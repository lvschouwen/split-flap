// WallLink.cpp — contract in WallLink.h.
#include "WallLink.h"

#include <WiFi.h>
#include <lwip/sockets.h>

#include "HelpersSerialHandling.h"
#include "TaskWatchdog.h"
#include "WallLinkCore.h"
#include "WallState.h"

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

WallLinkCore core;
WallRowsTable rowsTable;
uint32_t rowsGeneration = 0;
WallRowLink facts[WALL_LINK_MAX_ROWS];
bool factsDirty[WALL_LINK_MAX_ROWS] = {false};

int listenFd = -1;
uint32_t listenRetryAtMs = 0;

struct Socket {
  int fd = -1;
  bool broken = false;  // a write failed for good: closed at the end of the pass
  // The rest of a message the socket took only part of. Nothing else is
  // written to this connection until it is out.
  uint8_t tail[wl_ToRow_size + WL_PREFIX_MAX];
  size_t tailLen = 0;
};
Socket sockets[WALL_LINK_MAX_CONNS];

uint8_t readBuf[512];

const char* rowName(int row) {
  return row >= 0 && row < rowsTable.count ? rowsTable.rows[row].id : "?";
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
    f.everWelcomed = true;
    f.rescue = hello.rescue;
    f.reportedWidth = (uint8_t)hello.width;
    strlcpy(f.rev, hello.rev, sizeof(f.rev));
    f.connects++;
    if (restarted) f.restarts++;
    f.haveStatus = false;
    f.address[0] = 0;
    struct sockaddr_in peer = {};
    socklen_t len = sizeof(peer);
    if (lwip_getpeername(sockets[core.rows[row].conn].fd, (struct sockaddr*)&peer, &len) == 0) {
      lwip_inet_ntop(AF_INET, &peer.sin_addr, f.address, sizeof(f.address));
    }
    factsDirty[row] = true;
    SerialPrintf("link: %s connected from %s, rev %s, %d unit(s)%s%s\n", rowName(row), f.address,
                 f.rev, (int)f.reportedWidth, hello.rescue ? ", RESCUE MODE" : "",
                 restarted ? ", it restarted" : "");
  }

  void rowMessage(int row, const wl_ToMaster& message) override {
    if (message.which_body == wl_ToMaster_status_tag) {
      facts[row].status = message.body.status;
      facts[row].haveStatus = true;
    }
    factsDirty[row] = true;
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
    const int conn = ready ? core.accept(nowMs, hooks) : -1;
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
  hooks.note(conn, core.conns[conn].row, why);
  core.closed(conn);
  closeSocket(sockets[conn]);
}

void readRows(const fd_set& readable, uint32_t nowMs) {
  for (int conn = 0; conn < WALL_LINK_MAX_CONNS; conn++) {
    for (int i = 0; i < READS_PER_PASS; i++) {
      Socket& s = sockets[conn];
      if (s.fd < 0 || (i == 0 && !FD_ISSET(s.fd, &readable))) break;
      const int n = lwip_recv(s.fd, readBuf, sizeof(readBuf), MSG_DONTWAIT);
      if (n > 0) {
        core.bytes(conn, readBuf, (size_t)n, nowMs, hooks);
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

void publish(uint32_t nowMs) {
  static uint32_t contactPublishedAtMs = 0;
  const bool contactDue = wallLinkElapsed(nowMs, contactPublishedAtMs, CONTACT_PUBLISH_MS);
  if (contactDue) contactPublishedAtMs = nowMs;
  for (int row = 0; row < WALL_LINK_MAX_ROWS; row++) {
    if (contactDue && core.rows[row].conn >= 0) factsDirty[row] = true;
    if (!factsDirty[row]) continue;
    factsDirty[row] = false;
    facts[row].contact = core.rows[row].contact;
    wallStatePublishLink(row, facts[row]);
  }
}

void pass() {
  const uint32_t nowMs = millis();
  if (wallStateRowsGeneration() != rowsGeneration) {
    core.reset(hooks);
    rowsTable = wallStateRows(rowsGeneration);
    for (int row = 0; row < WALL_LINK_MAX_ROWS; row++) {
      facts[row] = WallRowLink{};
      factsDirty[row] = true;
    }
  }
  openListener(nowMs);
  if (listenFd < 0) {
    vTaskDelay(pdMS_TO_TICKS(PASS_WAIT_MS));
    return;
  }

  fd_set readable;
  FD_ZERO(&readable);
  FD_SET(listenFd, &readable);
  int maxFd = listenFd;
  for (const Socket& s : sockets) {
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
  if (FD_ISSET(listenFd, &readable)) acceptRows(nowMs);
  readRows(readable, nowMs);
  core.tick(nowMs, hooks);
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
