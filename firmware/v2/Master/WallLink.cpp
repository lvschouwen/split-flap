// WallLink.cpp — contract in WallLink.h.
#include "WallLink.h"

#include <WiFi.h>
#include <lwip/sockets.h>

#include <sys/time.h>

#include "ClockPolicy.h"  // clockIsTimeSynced
#include "FollowerImageStore.h"
#include "HelpersSerialHandling.h"
#include "LargeAlloc.h"
#include "SntpReply.h"
#include "TaskWatchdog.h"
#include "Tasks.h"
#include "EventRecord.h"
#include "WallJobs.h"
#include "WallLinkCore.h"
#include "WallRowEvents.h"
#include "WallShow.h"
#include "WallState.h"
#include "WallUpdatePolicy.h"
#include "WifiService.h"  // wifiNoteConfirmedTraffic

namespace {

// How long one pass waits for bytes. A text handed to the link goes out at
// the next pass, so this is spent out of its lead (400 ms for typed text).
constexpr uint32_t PASS_WAIT_MS = 20;
// After the listening socket could not be opened.
constexpr uint32_t LISTEN_RETRY_MS = 1000;
// Reads taken from one connection per pass, so one chatty row cannot hold the
// others (or the watchdog feed) off.
constexpr int READS_PER_PASS = 4;
// A connected row's contact times are published this often even when nothing
// else changed: a Pong moves "last heard", and readers judge reach from it.
constexpr uint32_t CONTACT_PUBLISH_MS = 1000;
// At most one log line per this about connections that are not a row.
constexpr uint32_t STRANGER_NOTE_MS = 10000;

char masterName[sizeof(((wl_Welcome*)0)->master_id)] = {0};

// The link's working state is some 15 KB: taken once from PSRAM in
// wallLinkInit (LargeAlloc.h), not from internal RAM.
WallLinkCore* core = nullptr;
WallRowsTable rowsTable;
uint32_t rowsGeneration = 0;
WallRowLink* facts = nullptr;  // WALL_LINK_MAX_ROWS
bool factsDirty[WALL_LINK_MAX_ROWS] = {false};
// How many entries each row may still add to the event record (WallRowEvents.h).
WallRowEventBudget eventBudget[WALL_LINK_MAX_ROWS];
bool eventBudgetLogged[WALL_LINK_MAX_ROWS] = {false};

int listenFd = -1;
uint32_t listenRetryAtMs = 0;
// The rows' time requests (SntpReply.h). Same task, same select.
int timeFd = -1;
// Requests answered per pass: a flood must not hold the rows off.
constexpr int TIME_REPLIES_PER_PASS = 4;

struct Socket {
  int fd = -1;
  bool broken = false;  // a write failed for good: closed at the end of the pass
  // The rest of a message the socket took only part of. Nothing else is
  // written to this connection until it is out.
  uint8_t tail[wl_ToRow_size + WL_PREFIX_MAX];
  size_t tailLen = 0;
};
Socket* sockets = nullptr;  // WALL_LINK_MAX_CONNS

uint8_t readBuf[512];

// A row's unit facts arrive in pieces; one buffer per row, taken the first
// time a row sends any and kept. The parsed result is too large for this
// task's stack.
char* unitsText[WALL_LINK_MAX_ROWS] = {nullptr};
WlDocAssembler* unitsAssembler[WALL_LINK_MAX_ROWS] = {nullptr};
UnitFactsDoc* unitsParsed = nullptr;

// ArduinoJson's tree for a unit facts document, outside internal RAM.
struct LargeAllocator : ArduinoJson::Allocator {
  void* allocate(size_t size) override { return largeAlloc(size); }
  void deallocate(void* pointer) override { free(pointer); }
  void* reallocate(void* pointer, size_t size) override {
    return heap_caps_realloc(pointer, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  }
};
LargeAllocator largeAllocator;

void takeUnitsPiece(int row, const wl_UnitsJson& piece, const char* name) {
  if (unitsText[row] == nullptr) {
    unitsText[row] = (char*)largeAlloc(UNIT_HEALTH_JSON_CAP);
    unitsAssembler[row] = (WlDocAssembler*)largeAlloc(sizeof(WlDocAssembler));
    if (unitsParsed == nullptr) unitsParsed = (UnitFactsDoc*)largeAlloc(sizeof(UnitFactsDoc));
    if (unitsText[row] == nullptr || unitsAssembler[row] == nullptr || unitsParsed == nullptr) {
      free(unitsText[row]);
      free(unitsAssembler[row]);
      unitsText[row] = nullptr;
      unitsAssembler[row] = nullptr;
      return;
    }
    new (unitsAssembler[row]) WlDocAssembler(unitsText[row], UNIT_HEALTH_JSON_CAP);
  }
  if (unitsAssembler[row]->add(piece) != WlDocAssembler::Result::Complete) return;
  const uint32_t nowMs = millis();
  if (unitFactsFromJson(unitsText[row], piece.total, nowMs, *unitsParsed, &largeAllocator)) {
    wallStatePublishUnits(row, *unitsParsed, nowMs);
  } else {
    SerialPrintf("link: %s: unit facts of %u bytes could not be read\n", name,
                 (unsigned)piece.total);
  }
}

const char* rowName(int row) {
  return row >= 0 && row < rowsTable.count ? rowsTable.rows[row].id : "?";
}

// ---- the stored row image, offered to rows on another rev (WallUpdatePolicy.h) ----

WallUpdater updater;
// The time of the pass being run. The offer rule is fed from the hooks and
// from the pass itself, and must see one clock: a later millis() in a hook
// would lie ahead of the pass's own "now" and read as a wrapped, huge age.
uint32_t passNowMs = 0;
FollowerImageFacts image;
bool imageKnown = false;
uint32_t imageGeneration = 0;

// "" while there is nothing to offer. A rev too long for the link's messages
// could never match what a row reports back, so such an image is not offered.
const char* offeredRev() {
  return imageKnown && strlen(image.rev) < sizeof(((wl_Update*)0)->rev) ? image.rev : "";
}

void updateEnded(int row, WallUpdateEnd how) {
  if (how == WallUpdateEnd::None || row < 0 || row >= WALL_LINK_MAX_ROWS) return;
  SerialPrintf("link: %s %s (rev %s, %u of %u failed offers%s)\n", rowName(row),
               wallUpdateEndText(how), image.rev, (unsigned)updater.attempts[row],
               (unsigned)WALL_UPDATE_ATTEMPT_CAP, updater.blocked[row] ? ", given up on" : "");
}

// A row's answer about a job the master holds open on it.
void takeOpState(const wl_OpState& state) {
  if (state.data.size > 0) {
    wallOpDataPut(state.op_id, state.data_offset, state.data.bytes, state.data.size);
  }
  char text[sizeof(((WallOp*)0)->detail)];
  switch (state.phase) {
    case wl_OpPhase_OP_RUNNING:
      return;
    case wl_OpPhase_OP_REFUSED:
      wallOpFinish(state.op_id, false, wallJobRefusalText(state.reason));
      return;
    case wl_OpPhase_OP_OK:
      wallJobOutcomeText(text, sizeof(text), true, 0, state.reason);
      wallOpFinish(state.op_id, true, text);
      return;
    default:
      wallJobOutcomeText(text, sizeof(text), false, state.outcome, state.reason);
      wallOpFinish(state.op_id, false, text);
      return;
  }
}

void closeSocket(Socket& s) {
  if (s.fd >= 0) lwip_close(s.fd);
  s = Socket{};
}

// "Not now": the socket's buffer is full, or lwIP is short of memory for the
// moment (a row that is not reading is exactly when that happens).
bool wouldBlock(int error) {
  return error == EAGAIN || error == EWOULDBLOCK || error == ENOMEM || error == ENOBUFS;
}

// True when nothing is left waiting. Marks the socket broken on a real error.
bool flushTail(Socket& s) {
  if (s.tailLen == 0) return true;
  const int sent = lwip_send(s.fd, s.tail, s.tailLen, MSG_DONTWAIT);
  if (sent < 0) {
    if (!wouldBlock(errno)) s.broken = true;
    return false;
  }
  memmove(s.tail, s.tail + sent, s.tailLen - sent);
  s.tailLen -= sent;
  return s.tailLen == 0;
}

struct Hooks : WallLinkHooks {
  int rowForId(const char* id) override { return wallRowsFind(rowsTable, id); }
  const char* masterId() override { return masterName; }

  bool write(int conn, const uint8_t* data, size_t n) override {
    Socket& s = sockets[conn];
    if (s.fd < 0 || s.broken || !flushTail(s)) return false;
    const int sent = lwip_send(s.fd, data, n, MSG_DONTWAIT);
    if (sent < 0) {
      if (!wouldBlock(errno)) s.broken = true;
      return false;
    }
    if ((size_t)sent < n) {
      s.tailLen = n - sent;
      memcpy(s.tail, data + sent, s.tailLen);
    }
    return true;
  }

  void close(int conn) override { closeSocket(sockets[conn]); }

  bool samePeer(int a, int b) override {
    struct sockaddr_in pa = {}, pb = {};
    socklen_t la = sizeof(pa), lb = sizeof(pb);
    return lwip_getpeername(sockets[a].fd, (struct sockaddr*)&pa, &la) == 0 &&
           lwip_getpeername(sockets[b].fd, (struct sockaddr*)&pb, &lb) == 0 &&
           pa.sin_addr.s_addr == pb.sin_addr.s_addr;
  }

  bool fromPairedAddress(int conn, int row) override {
    struct sockaddr_in peer = {};
    socklen_t len = sizeof(peer);
    uint8_t paired[4];
    if (row < 0 || row >= rowsTable.count || !wallRowHostParse(rowsTable.rows[row].host, paired) ||
        lwip_getpeername(sockets[conn].fd, (struct sockaddr*)&peer, &len) != 0) {
      return false;
    }
    return memcmp(&peer.sin_addr.s_addr, paired, sizeof(paired)) == 0;
  }

  void rowHello(int row, const wl_Hello& hello, bool restarted) override {
    WallRowLink& f = facts[row];
    // Before the offer rule hears of the Hello: a rev that changed while no
    // offer was out to this row.
    updater.noteRev(row, f.rev, hello.rev);
    updateEnded(row, updater.hello(row, hello.rev, hello.rescue, restarted, offeredRev(), passNowMs));
    f.everWelcomed = true;
    f.rescue = hello.rescue;
    f.reportedWidth = (uint8_t)hello.width;
    strlcpy(f.rev, hello.rev, sizeof(f.rev));
    f.connects++;
    if (restarted) {
      f.restarts++;
      wallOpsFailRow(row, "the row restarted");
    }
    if (unitsAssembler[row] != nullptr) unitsAssembler[row]->active = false;
    f.haveStatus = false;
    f.worstLateMs = 0;
    f.address[0] = 0;
    struct sockaddr_in peer = {};
    socklen_t len = sizeof(peer);
    if (lwip_getpeername(sockets[core->rows[row].conn].fd, (struct sockaddr*)&peer, &len) == 0) {
      lwip_inet_ntop(AF_INET, &peer.sin_addr, f.address, sizeof(f.address));
    }
    factsDirty[row] = true;
    SerialPrintf("link: %s connected from %s, rev %s, %d unit(s)%s%s\n", rowName(row), f.address,
                 f.rev, (int)f.reportedWidth, hello.rescue ? ", RESCUE MODE" : "",
                 restarted ? ", it restarted" : "");
  }

  void rowMessage(int row, const wl_ToMaster& message) override {
    // #515: a paired row answering is traffic that went both ways.
    wifiNoteConfirmedTraffic();
    if (message.which_body == wl_ToMaster_status_tag) {
      facts[row].status = message.body.status;
      facts[row].haveStatus = true;
    } else if (message.which_body == wl_ToMaster_shown_tag) {
      WallRowLink& f = facts[row];
      f.shownCount++;
      f.lastLateMs = message.body.shown.late_ms;
      if (f.lastLateMs > f.worstLateMs) f.worstLateMs = f.lastLateMs;
    } else if (message.which_body == wl_ToMaster_units_json_tag) {
      takeUnitsPiece(row, message.body.units_json, rowName(row));
    } else if (message.which_body == wl_ToMaster_op_state_tag) {
      takeOpState(message.body.op_state);
    } else if (message.which_body == wl_ToMaster_update_state_tag) {
      const wl_UpdateState& u = message.body.update_state;
      SerialPrintf("link: %s: update to %s: phase %d, reason %d, detail %u\n", rowName(row), u.rev,
                   (int)u.phase, (int)u.reason, (unsigned)u.detail);
      updateEnded(row, updater.answer(row, u, offeredRev(), passNowMs));
    } else if (message.which_body == wl_ToMaster_event_tag) {
      // #570: what the row says happened on it goes into the event record.
      const wl_Event& e = message.body.event;
      if (eventBudget[row].take(passNowMs)) {
        eventBudgetLogged[row] = false;
        eventRecord(EventKind::RowEvent, wallRowEventDetail(e.code), rowName(row),
                    e.unit <= 255 ? (uint8_t)e.unit : 0, e.a, e.b, e.age_s);
      } else if (!eventBudgetLogged[row]) {
        eventBudgetLogged[row] = true;
        SerialPrintf("link: %s sends events faster than they are recorded — dropping\n",
                     rowName(row));
      }
    }
    factsDirty[row] = true;
  }

  void jobEnded(int row, uint32_t opId, WallJobEnd why) override {
    (void)row;
    wallOpFinish(opId, false,
                 why == WallJobEnd::NotHandedOver ? "the row did not take the job in time"
                                                  : "no result from the row in time");
  }

  void note(int conn, int row, const char* what) override {
    (void)conn;
    if (row >= 0) {
      SerialPrintf("link: %s: %s\n", rowName(row), what);
      factsDirty[row] = true;
      return;
    }
    // Anyone on the network can open connections that are nobody: their
    // lines are held to one per interval, so they cannot fill the log.
    const uint32_t nowMs = millis();
    if (strangerNotes != 0 && !wallLinkElapsed(nowMs, strangerNoteAtMs, STRANGER_NOTE_MS)) {
      strangerNotesHeld++;
      return;
    }
    SerialPrintf("link: a connection: %s (%u more since the last such line)\n", what,
                 (unsigned)strangerNotesHeld);
    strangerNotes++;
    strangerNotesHeld = 0;
    strangerNoteAtMs = nowMs;
  }

  uint32_t strangerNotes = 0;
  uint32_t strangerNotesHeld = 0;
  uint32_t strangerNoteAtMs = 0;
};
Hooks hooks;

void openListener(uint32_t nowMs) {
  if (listenFd >= 0 || !wallLinkElapsed(nowMs, listenRetryAtMs, LISTEN_RETRY_MS)) return;
  listenRetryAtMs = nowMs;
  const int fd = lwip_socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return;
  int on = 1;
  lwip_setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
  struct sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(WALL_LINK_PORT);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  const int flags = lwip_fcntl(fd, F_GETFL, 0);
  if (flags < 0 || lwip_fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 ||
      lwip_bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0 ||
      lwip_listen(fd, WALL_LINK_MAX_CONNS) < 0) {
    SerialPrintf("link: cannot listen on port %d (errno %d)\n", (int)WALL_LINK_PORT, errno);
    lwip_close(fd);
    return;
  }
  listenFd = fd;
  SerialPrintf("link: listening on port %d\n", (int)WALL_LINK_PORT);
}

void openTimeSocket() {
  if (timeFd >= 0) return;
  const int fd = lwip_socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) return;
  struct sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(SNTP_PORT);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  const int flags = lwip_fcntl(fd, F_GETFL, 0);
  if (flags < 0 || lwip_fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 ||
      lwip_bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    lwip_close(fd);
    return;
  }
  timeFd = fd;
  SerialPrintf("link: answering time requests on port %d\n", (int)SNTP_PORT);
}

uint64_t epochNowUs(bool& synced) {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  synced = clockIsTimeSynced(tv.tv_sec);
  return (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
}

// A master without synced time stays silent: it names no flip instants
// either, and a wrong time is worse for a row than none.
void answerTime() {
  for (int i = 0; i < TIME_REPLIES_PER_PASS; i++) {
    struct sockaddr_in from = {};
    socklen_t fromLen = sizeof(from);
    uint8_t request[SNTP_PACKET_LEN];
    const int n = lwip_recvfrom(timeFd, request, sizeof(request), MSG_DONTWAIT,
                                (struct sockaddr*)&from, &fromLen);
    if (n < 0) return;
    bool synced = false;
    const uint64_t rxUs = epochNowUs(synced);
    uint8_t reply[SNTP_PACKET_LEN];
    if (!synced || !sntpBuildReply(request, (size_t)n, rxUs, epochNowUs(synced), reply)) continue;
    lwip_sendto(timeFd, reply, sizeof(reply), MSG_DONTWAIT, (struct sockaddr*)&from, fromLen);
  }
}

void acceptRows(uint32_t nowMs) {
  for (;;) {
    const int fd = lwip_accept(listenFd, nullptr, nullptr);
    if (fd < 0) return;
    int on = 1;
    int idle = WALL_LINK_KEEPALIVE_IDLE_S;
    int interval = WALL_LINK_KEEPALIVE_INTERVAL_S;
    int count = WALL_LINK_KEEPALIVE_COUNT;
    const int flags = lwip_fcntl(fd, F_GETFL, 0);
    const bool ready =
        flags >= 0 && lwip_fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0 &&
        lwip_setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on)) == 0 &&
        lwip_setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle)) == 0 &&
        lwip_setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval)) == 0 &&
        lwip_setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count)) == 0;
    // Messages are small and each is wanted now: do not wait to fill a segment.
    lwip_setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
    const int conn = ready ? core->accept(nowMs, hooks) : -1;
    if (conn < 0) {
      hooks.note(-1, -1, ready ? "refused, no room" : "refused, socket options failed");
      lwip_close(fd);
      continue;
    }
    sockets[conn] = Socket{};
    sockets[conn].fd = fd;
  }
}

// The socket of `conn` is gone or useless: tell the core, then close it.
void lost(int conn, const char* why) {
  hooks.note(conn, core->conns[conn].row, why);
  core->closed(conn);
  closeSocket(sockets[conn]);
}

void readRows(const fd_set& readable, uint32_t nowMs) {
  for (int conn = 0; conn < WALL_LINK_MAX_CONNS; conn++) {
    for (int i = 0; i < READS_PER_PASS; i++) {
      Socket& s = sockets[conn];
      if (s.fd < 0 || (i == 0 && !FD_ISSET(s.fd, &readable))) break;
      const int n = lwip_recv(s.fd, readBuf, sizeof(readBuf), MSG_DONTWAIT);
      if (n > 0) {
        core->bytes(conn, readBuf, (size_t)n, nowMs, hooks);
        continue;
      }
      if (n == 0) {
        lost(conn, "connection closed by the row");
      } else if (!wouldBlock(errno)) {
        lost(conn, "connection lost");
      }
      break;
    }
  }
}

// What the rows are to show and how they are to behave, handed to the core,
// which sends it when each row can take it.
void showAndSettings(uint32_t nowMs) {
  static uint32_t settingsGeneration = 0;
  static WallRowShow show;
  wallShowServiceOwnRow(nowMs);
  for (int row = 0; row < rowsTable.count; row++) {
    if (wallShowTakeRow(row, show)) core->setText(row, show.text, show.speed, show.commitAtMs);
  }
  core->setQuiet(tasksQuiet());
  if (wallStateRowSettingsGeneration() != settingsGeneration) {
    static wl_Config settings;
    settingsGeneration = wallStateRowSettings(settings);
    core->setConfig(settings);
  }
}

// Jobs the web side staged for a row board go to the core, which writes each
// when its row can take it.
void takeJobs(uint32_t nowMs) {
  static wl_Op op;
  // A job staged against a newer table than this pass runs on names its row
  // by that table's numbers: it waits for the pass that has taken the table.
  if (wallStateRowsGeneration() != rowsGeneration) return;
  for (int row = 0; row < rowsTable.count && row < WALL_LINK_MAX_ROWS; row++) {
    if (!wallJobTake(row, op)) continue;
    // An update ends in the row's restart, which would cut the job short.
    const bool updating = updater.phase != WallUpdatePhase::Idle && updater.row == row;
    if (updating) {
      wallOpFinish(op.op_id, false, "the row is taking a firmware update");
    } else if (!core->startJob(row, op, nowMs)) {
      wallOpFinish(op.op_id, false, "another unit job holds the row");
    }
  }
}

void offerImage(uint32_t nowMs) {
  if (followerImageFactsGeneration() != imageGeneration) {
    imageGeneration = followerImageFactsGeneration();
    imageKnown = followerImageFacts(image);
    updater.newImage();
    if (imageKnown) {
      SerialPrintf("link: row image to offer: rev %s, %u bytes%s\n", image.rev,
                   (unsigned)image.size, image.packed ? ", packed" : "");
    }
  }
  // Asked for by the current table's row numbers: left waiting for the pass
  // that has taken that table.
  const uint32_t retries =
      wallStateRowsGeneration() == rowsGeneration ? wallStateTakeUpdateRetries() : 0;
  for (int row = 0; row < rowsTable.count && row < WALL_LINK_MAX_ROWS; row++) {
    if (retries & (1UL << row)) updater.retry(row);
  }
  if (updater.phase != WallUpdatePhase::Idle) {
    const int offeredTo = updater.row;  // tick() forgets it when the offer ends
    updateEnded(offeredTo, updater.tick(nowMs));
  }

  static WallUpdateRow views[WALL_LINK_MAX_ROWS];
  for (int row = 0; row < WALL_LINK_MAX_ROWS; row++) {
    const WallLinkRow& r = core->rows[row];
    const bool welcomed = row < rowsTable.count && r.conn >= 0 && r.contact.helloSeen;
    views[row].reachable = welcomed && !r.contact.busy && r.job.stage == WallJobStage::None;
    views[row].rescue = facts[row].rescue;
    views[row].rev = facts[row].rev;
    updater.health(row, welcomed && !facts[row].rescue, nowMs);
    if (facts[row].updateAttempts != updater.attempts[row] ||
        facts[row].updateBlocked != updater.blocked[row]) {
      facts[row].updateAttempts = updater.attempts[row];
      facts[row].updateBlocked = updater.blocked[row];
      factsDirty[row] = true;
    }
  }
  const int row = updater.nextCandidate(views, WALL_LINK_MAX_ROWS, offeredRev(), nowMs);
  if (row >= 0) {
    static wl_ToRow offer;
    wlClear(offer);
    offer.which_body = wl_ToRow_update_tag;
    wl_Update& u = offer.body.update;
    strlcpy(u.rev, image.rev, sizeof(u.rev));
    u.size = image.size;
    memcpy(u.md5, image.md5, sizeof(u.md5));
    u.packed = image.packed;
    // http_port stays 0: the web server's own port.
    if (core->send(row, offer, nowMs, hooks)) {
      updater.offered(row, views[row].rescue, nowMs);
      SerialPrintf("link: %s runs %s%s: offered the stored image, rev %s\n", rowName(row),
                   views[row].rev, views[row].rescue ? " in RESCUE MODE" : "", image.rev);
    }
  }
  static uint8_t publishedPhase = 0xFF;
  static int publishedRow = -2;
  if ((uint8_t)updater.phase != publishedPhase || updater.row != publishedRow) {
    publishedPhase = (uint8_t)updater.phase;
    publishedRow = updater.row;
    wallStatePublishUpdate(publishedPhase, publishedRow);
  }
}

void publish(uint32_t nowMs) {
  static uint32_t contactPublishedAtMs = 0;
  const bool contactDue = wallLinkElapsed(nowMs, contactPublishedAtMs, CONTACT_PUBLISH_MS);
  if (contactDue) contactPublishedAtMs = nowMs;
  for (int row = 0; row < WALL_LINK_MAX_ROWS; row++) {
    if (contactDue && core->rows[row].conn >= 0) factsDirty[row] = true;
    if (!factsDirty[row]) continue;
    factsDirty[row] = false;
    facts[row].contact = core->rows[row].contact;
    facts[row].textApplied = wallRowTextApplied(core->rows[row].text);
    wallStatePublishLink(row, facts[row]);
  }
}

void pass() {
  const uint32_t nowMs = millis();
  passNowMs = nowMs;
  if (wallStateRowsGeneration() != rowsGeneration) {
    core->reset(hooks);
    updater.reset();
    rowsTable = wallStateRows(rowsGeneration);
    wallShowRowsChanged(rowsTable);
    for (int row = 0; row < WALL_LINK_MAX_ROWS; row++) {
      facts[row] = WallRowLink{};
      factsDirty[row] = true;
    }
  }
  openListener(nowMs);
  if (listenFd >= 0) openTimeSocket();
  if (listenFd < 0) {
    vTaskDelay(pdMS_TO_TICKS(PASS_WAIT_MS));
    return;
  }

  fd_set readable;
  FD_ZERO(&readable);
  FD_SET(listenFd, &readable);
  int maxFd = listenFd;
  if (timeFd >= 0) {
    FD_SET(timeFd, &readable);
    if (timeFd > maxFd) maxFd = timeFd;
  }
  for (int conn = 0; conn < WALL_LINK_MAX_CONNS; conn++) {
    const Socket& s = sockets[conn];
    if (s.fd < 0) continue;
    FD_SET(s.fd, &readable);
    if (s.fd > maxFd) maxFd = s.fd;
  }
  struct timeval wait = {};
  wait.tv_usec = PASS_WAIT_MS * 1000;
  if (lwip_select(maxFd + 1, &readable, nullptr, nullptr, &wait) < 0) {
    vTaskDelay(pdMS_TO_TICKS(PASS_WAIT_MS));
    FD_ZERO(&readable);
  }
  if (timeFd >= 0 && FD_ISSET(timeFd, &readable)) answerTime();
  if (FD_ISSET(listenFd, &readable)) acceptRows(nowMs);
  readRows(readable, nowMs);
  showAndSettings(nowMs);
  takeJobs(nowMs);
  core->tick(nowMs, hooks);
  offerImage(nowMs);
  // Asked for by the current table's row numbers, like an update retry. Said
  // once: a row that cannot be written to now is not restarted later, when
  // nobody expects it any more.
  const uint32_t restarts =
      wallStateRowsGeneration() == rowsGeneration ? wallStateTakeRestarts() : 0;
  for (int row = 0; row < rowsTable.count && row < WALL_LINK_MAX_ROWS; row++) {
    if (!(restarts & (1UL << row))) continue;
    static wl_ToRow restart;
    wlClear(restart);
    restart.which_body = wl_ToRow_restart_tag;
    SerialPrintf("link: %s %s\n", rowsTable.rows[row].id,
                 core->send(row, restart, nowMs, hooks) ? "told to restart"
                                                        : "could not be told to restart");
  }
  char releasing[WALL_ROW_ID_MAX + 1];
  if (wallStateReleaseAsked(releasing, sizeof(releasing))) {
    const int row = wallRowsFind(rowsTable, releasing);
    if (row >= 0) {
      static wl_ToRow release;
      wlClear(release);
      release.which_body = wl_ToRow_release_tag;
      if (core->send(row, release, nowMs, hooks)) SerialPrintf("link: %s released\n", releasing);
    }
    wallStateReleaseAnswered();
  }
  for (int conn = 0; conn < WALL_LINK_MAX_CONNS; conn++) {
    Socket& s = sockets[conn];
    if (s.fd < 0) continue;
    flushTail(s);
    if (s.broken) lost(conn, "connection lost while writing");
  }
  publish(nowMs);
}

}  // namespace

void wallLinkInit(const String& masterId) {
  strlcpy(masterName, masterId.c_str(), sizeof(masterName));
  core = (WallLinkCore*)largeAlloc(sizeof(WallLinkCore));
  facts = (WallRowLink*)largeAlloc(sizeof(WallRowLink) * WALL_LINK_MAX_ROWS);
  sockets = (Socket*)largeAlloc(sizeof(Socket) * WALL_LINK_MAX_CONNS);
  if (core == nullptr || facts == nullptr || sockets == nullptr) {
    Serial.println(F("FATAL: link state allocation failed"));
    abort();
  }
  new (core) WallLinkCore();
  for (int i = 0; i < WALL_LINK_MAX_ROWS; i++) new (&facts[i]) WallRowLink();
  for (int i = 0; i < WALL_LINK_MAX_CONNS; i++) new (&sockets[i]) Socket();
  rowsTable = wallStateRows(rowsGeneration);
}

void wallLinkTaskMain(void*) {
  SerialPrintf("linkTask up on core %d\n", xPortGetCoreID());
  if (esp_err_t e = wdtSubscribeSelf(); e != ESP_OK)
    SerialPrintf("wdt: link subscribe -> %s\n", esp_err_to_name(e));
  for (;;) {
    wdtFeed();
    pass();
  }
}
