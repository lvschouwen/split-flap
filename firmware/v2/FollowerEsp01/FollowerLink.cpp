// FollowerLink.cpp — socket glue of the wall link on the row board. Timing
// rules in FollowerLinkPolicy.h; pairing, phase machine, flip timing and
// fallback clock are FollowerCluster's. Unit jobs go through the one staged
// slot (FollowerUnitJobs.h); which checks a job takes is FollowerLinkOps.h.
// An offered image is fetched by FollowerUpdate.cpp.
#include "FollowerLink.h"

#include <Arduino.h>
#include <ESP8266WiFi.h>

#include "BuildVersion.h"
#include "FollowerBus.h"
#include "FollowerBusRecovery.h"
#include "FollowerCluster.h"
#include "FollowerEscalation.h"
#include "FollowerLinkOps.h"
#include "FollowerLinkPolicy.h"
#include "FollowerLog.h"
#include "FollowerMem.h"
#include "FollowerPrefs.h"
#include "FollowerRescue.h"
#include "FollowerUnitJobs.h"
#include "FollowerUpdate.h"
#include "FollowerUpdatePolicy.h"
#include "FollowerWeb.h"
#include "FollowerWifi.h"
#include "WallLinkStream.h"

namespace {

WiFiClient sock;
WlRowReader reader;
// One outgoing message at a time. Static: wl_ToMaster holds the largest
// payload (480 B) and loop()'s stack is 4 KB.
wl_ToMaster out;
uint8_t wire[WL_PREFIX_MAX + wl_ToMaster_size];

bool welcomed = false;
uint32_t bootId = 0;
uint32_t connectCount = 0;
uint32_t dropCount = 0;
uint32_t backoffMs = 0;
uint32_t nextDialMs = 0;
uint32_t connectedAtMs = 0;
uint32_t lastStatusMs = 0;
bool busySent = false;

// The one unit job the master has running here. It outlives a dropped
// connection: the job keeps running and its end is reported on the next one.
struct LinkOp {
  bool active = false;
  uint32_t opId = 0;
  uint32_t seq = 0;  // its name in the staged-op and result slots
  FollowerOpKind kind = FollowerOpKind::None;
};
LinkOp linkOp;

// The unit facts document being sent, one piece per loop pass. A buffer of
// FollowerMem.h, held only while it travels.
char* unitsDoc = nullptr;
uint32_t unitsTotal = 0;
uint32_t unitsOffset = 0;
uint32_t unitsDocId = 0;
uint32_t lastUnitsMs = 0;
bool unitsDue = false;

// The image the master last offered, taken up after the messages in hand
// are read; and what became of the last one, kept until the master has it.
wl_Update offer;
bool offerPending = false;
wl_UpdateState updateReport;
bool updateReportPending = false;

// The log goes up while the master asks for it (LogCtl), per connection. The
// cursor outlives a connection: lines logged while the master was away
// follow the ones it already has.
bool logOn = false;
uint32_t logCursor = 0;

// Who the current connection was opened to. A pairing that changes under it
// (POST /pair) closes it: the row then dials the master it now obeys.
char dialledId[33];
char dialledHost[48];

static_assert((int)FollowerFallback::Blank == wl_Fallback_FALLBACK_BLANK &&
                  (int)FollowerFallback::Time == wl_Fallback_FALLBACK_TIME &&
                  (int)FollowerFallback::Date == wl_Fallback_FALLBACK_DATE,
              "FollowerFallback is stored with the numbers of wl.Fallback");

void releaseUnitsDoc() {
  followerBufFree(unitsDoc);
  unitsDoc = nullptr;
}

void drop(const __FlashStringHelper* why) {
  if (sock.connected() || welcomed) {
    SerialPrint(F("link: closed — "));
    SerialPrintln(why);
  }
  sock.stop();
  reader.reset();
  releaseUnitsDoc();
  welcomed = false;
  logOn = false;
  offerPending = false;  // the master offers again on the next connection
  dropCount++;
  backoffMs = followerLinkNextBackoffMs(backoffMs);
  nextDialMs = millis() + backoffMs;
}

// A message that cannot be written means a master that stopped reading:
// the connection is closed rather than left to block loop() on every write.
bool send() {
  const size_t n = wlEncodeToMaster(wire, sizeof(wire), out);
  if (n != 0 && sock.write(wire, n) == n) return true;
  drop(F("write failed"));
  return false;
}

void sendHello() {
  wlClear(out);
  out.which_body = wl_ToMaster_hello_tag;
  wl_Hello& h = out.body.hello;
  h.protocol = WALL_LINK_PROTOCOL;
  strlcpy(h.id, effectiveDeviceName.c_str(), sizeof(h.id));
  strlcpy(h.rev, GIT_REV, sizeof(h.rev));
  h.boot_id = bootId;
  h.rescue = rescueActive();
  h.width = (uint32_t)displayWidth;
  send();
}

void sendStatus(bool timeSynced) {
  const BusRecoveryState& bus = followerBusRecovery();
  wlClear(out);
  out.which_body = wl_ToMaster_status_tag;
  wl_Status& s = out.body.status;
  s.up_s = millis() / 1000;
  s.heap = ESP.getFreeHeap();
  s.min_heap = followerMinHeap();
  s.max_block = ESP.getMaxFreeBlockSize();
  s.heap2 = followerSecondHeapFree();
  s.rssi = WiFi.RSSI();
  s.tx_power = (uint32_t)(followerTxPowerDbm10() * 4 / 10);
  s.bus_tx = followerBusTxCount();
  s.bus_err = followerBusErrCount();
  s.bus_dead = bus.dead;
  s.bus_episodes = bus.episodes;
  s.escalations = escalationCount(escalationRecordGet());
  s.busy = unitOpsBusy();
  s.image_size = ESP.getSketchSize();
  s.time_synced = timeSynced;
  if (send()) {
    lastStatusMs = millis();
    busySent = s.busy;
  }
}

// An OpState without result data; `out` is left ready for a caller to add some.
void fillOpState(uint32_t opId, wl_OpPhase phase, uint32_t reason, uint32_t outcome) {
  wlClear(out);
  out.which_body = wl_ToMaster_op_state_tag;
  wl_OpState& o = out.body.op_state;
  o.op_id = opId;
  o.phase = phase;
  o.reason = reason;
  o.outcome = outcome;
}

void refuseOp(uint32_t opId, wl_OpRefusal why) {
  fillOpState(opId, wl_OpPhase_OP_REFUSED, (uint32_t)why, 0);
  send();
}

void handleOp(const wl_Op& o) {
  if (rescueActive()) {
    refuseOp(o.op_id, wl_OpRefusal_REFUSAL_RESCUE);
    return;
  }
  if (linkOp.active) {
    refuseOp(o.op_id, wl_OpRefusal_REFUSAL_BUSY);
    return;
  }
  const FollowerLinkOpPlan plan =
      followerLinkPlanOp((uint32_t)o.opcode, o.address, (long)o.arg, unitFacts, UNITS_AMOUNT);
  if (plan.refusal != wl_OpRefusal_REFUSAL_NONE) {
    refuseOp(o.op_id, plan.refusal);
    return;
  }
  uint32_t seq = 0;
  switch (unitOpStage(plan.kind, plan.addr, plan.arg, seq)) {
    case UnitOpStaged::Rescue:
      refuseOp(o.op_id, wl_OpRefusal_REFUSAL_RESCUE);
      return;
    case UnitOpStaged::Busy:
      refuseOp(o.op_id, wl_OpRefusal_REFUSAL_BUSY);
      return;
    case UnitOpStaged::NoMemory:
      refuseOp(o.op_id, wl_OpRefusal_REFUSAL_NO_MEMORY);
      return;
    case UnitOpStaged::Yes:
      break;
  }
  releaseUnitsDoc();  // a unit update is this board's memory low point
  unitsDue = true;
  linkOp.active = true;
  linkOp.opId = o.op_id;
  linkOp.seq = seq;
  linkOp.kind = plan.kind;
  fillOpState(o.op_id, wl_OpPhase_OP_RUNNING, 0, 0);
  send();
}

// Reports the end of the master's job once its result is in the slot. A
// failed send (which closes the connection) leaves the job open, so the end
// goes out again on the next one.
void opTick() {
  if (!linkOp.active) return;
  const MaintResult& result = unitOpResult();
  const OpResultState state = opResultQuery(result, linkOp.seq);
  if (state == OpResultState::Pending) return;

  bool sent = false;
  if (state == OpResultState::Expired) {
    // Another job has used the result slot since; how this one ended is gone.
    fillOpState(linkOp.opId, wl_OpPhase_OP_FAILED, 0, (uint32_t)MaintOutcome::Pending);
    sent = send();
  } else {
    const wl_OpPhase phase = followerLinkPhaseFor(result.outcome);
    const uint32_t reason = (uint32_t)result.reason;
    const uint32_t outcome = phase == wl_OpPhase_OP_OK ? 0 : (uint32_t)result.outcome;
    const uint8_t* dump = linkOp.kind == FollowerOpKind::BootDump && phase == wl_OpPhase_OP_OK
                              ? unitOpBootDumpBytes(linkOp.seq)
                              : nullptr;
    if (dump == nullptr && linkOp.kind == FollowerOpKind::BootDump && phase == wl_OpPhase_OP_OK) {
      // Read, but the bytes were given back before they could be sent.
      fillOpState(linkOp.opId, wl_OpPhase_OP_FAILED, 0, (uint32_t)MaintOutcome::Pending);
      sent = send();
    } else if (dump != nullptr) {
      sent = true;
      for (uint32_t offset = 0; sent && offset < BOOT_SECTION_LEN;) {
        const uint32_t n = followerLinkPieceLen(BOOT_SECTION_LEN, offset,
                                                sizeof(out.body.op_state.data.bytes));
        const bool last = offset + n >= BOOT_SECTION_LEN;
        fillOpState(linkOp.opId, last ? wl_OpPhase_OP_OK : wl_OpPhase_OP_RUNNING, reason, 0);
        wl_OpState& o = out.body.op_state;
        o.data_offset = offset;
        memcpy(o.data.bytes, dump + offset, n);
        o.data.size = (pb_size_t)n;
        sent = send();
        offset += n;
      }
    } else {
      fillOpState(linkOp.opId, phase, reason, outcome);
      wl_OpState& o = out.body.op_state;
      char* text = (char*)o.data.bytes;
      if (linkOp.kind == FollowerOpKind::SelfTest) {
        buildSelfTestJson(text, 128, unitOpSelfTest(), linkOp.seq);
        o.data.size = (pb_size_t)strlen(text);
      } else if (linkOp.kind == FollowerOpKind::BootInfo) {
        o.data.size = (pb_size_t)buildBootInfoJson(text, BOOT_INFO_JSON_CAP, unitOpBootInfo(),
                                                    linkOp.seq);
      }
      sent = send();
    }
  }
  if (!sent) return;
  linkOp.active = false;
  unitsDue = true;  // what the job changed
}

void reportUpdate(wl_UpdatePhase phase, wl_UpdateReason reason, uint32_t detail) {
  memset(&updateReport, 0, sizeof(updateReport));
  strlcpy(updateReport.rev, offer.rev, sizeof(updateReport.rev));
  updateReport.phase = phase;
  updateReport.reason = reason;
  updateReport.detail = detail;
  updateReportPending = true;
}

bool sendUpdateReport() {
  wlClear(out);
  out.which_body = wl_ToMaster_update_state_tag;
  out.body.update_state = updateReport;
  return send();
}

// Takes up an offered image: refused at once, or fetched and stored while
// loop() waits here. The master is told before the silence and after it; an
// answer that did not get out goes on the next connection.
void updateTick(const FollowerClusterView& view) {
  if (offerPending) {
    offerPending = false;
    const uint32_t maxSpace = followerUpdateMaxSpace(ESP.getFreeSketchSpace());
    const wl_UpdateReason refusal = followerUpdateAdmit(
        offer, GIT_REV, rescueActive(), unitOpsBusy() || linkOp.active, maxSpace);
    char host[48];
    if (refusal != wl_UpdateReason_UPDATE_REASON_NONE) {
      reportUpdate(wl_UpdatePhase_UPDATE_REFUSED, refusal,
                   refusal == wl_UpdateReason_UPDATE_TOO_LARGE ? maxSpace : 0);
    } else if (!followerLinkHostPart(view.leaderHost.c_str(), host, sizeof(host))) {
      reportUpdate(wl_UpdatePhase_UPDATE_REFUSED, wl_UpdateReason_UPDATE_UNREACHABLE, 0);
    } else {
      releaseUnitsDoc();  // the download needs the memory
      reportUpdate(wl_UpdatePhase_UPDATE_DOWNLOADING, wl_UpdateReason_UPDATE_REASON_NONE, 0);
      if (!sendUpdateReport()) {
        // The master never heard that this row goes silent; it offers again.
        updateReportPending = false;
        return;
      }
      const FollowerUpdateResult result = updateDownloadAndInstall(offer, host);
      reportUpdate(result.phase, result.reason, result.detail);
      if (result.phase == wl_UpdatePhase_UPDATE_INSTALLED) isPendingReboot = true;
    }
  }
  if (updateReportPending && welcomed && sendUpdateReport()) updateReportPending = false;
}

// The unit facts, on connect, after a job and every 30 s. Not started while
// a job runs or a text waits for its flip instant.
void unitsTick() {
  if (rescueActive()) return;  // rescue mode never reads the units
  if (unitsDoc == nullptr) {
    if (!unitsDue && !followerLinkElapsed(millis(), lastUnitsMs, FOLLOWER_LINK_UNITS_INTERVAL_MS)) {
      return;
    }
    if (unitOpsBusy() || clusterRenderPending()) return;
    const size_t cap = followerHealthBufCap(displayWidth, UNITS_AMOUNT);
    lastUnitsMs = millis();
    unitsDue = false;
    unitsDoc = (char*)followerBufAlloc(cap);
    if (unitsDoc == nullptr) return;
    unitsTotal = (uint32_t)unitsHealthJson(unitsDoc, cap);
    unitsOffset = 0;
    unitsDocId++;
    if (unitsTotal == 0) {
      releaseUnitsDoc();
      return;
    }
  }
  const uint32_t next = wlUnitsPiece(out, unitsDocId, unitsDoc, unitsTotal, unitsOffset);
  if (!send()) return;
  unitsOffset = next;
  if (unitsOffset >= unitsTotal) releaseUnitsDoc();
}

// One log line per loop pass, so a full ring does not hold up a text.
void logTick() {
  if (!logOn) return;
  wlClear(out);
  wl_LogLine& l = out.body.log_line;
  size_t len = 0;
  uint32_t next = logCursor;
  if (!followerLogRing().nextLine(next, (char*)l.text.bytes, sizeof(l.text.bytes), len)) {
    return;
  }
  out.which_body = wl_ToMaster_log_line_tag;
  l.text.size = (pb_size_t)len;
  // A line that did not get out is sent again on the next connection.
  if (send()) logCursor = next;
}

void handle(const wl_ToRow& m, const FollowerClusterView& view) {
  if (!welcomed) {
    if (m.which_body != wl_ToRow_welcome_tag ||
        !followerLinkMasterAccepted(view.leaderName.c_str(), m.body.welcome.master_id)) {
      drop(F("not the master this row is paired with"));
      return;
    }
    welcomed = true;
    connectCount++;
    backoffMs = 0;
    // A new connection is a new render sequence: the master numbers renders
    // per connection, and the phase machine accepts any new epoch.
    clusterMasterConnected(bootId + connectCount);
    SerialPrintln(F("link: connected to the master"));
    sendStatus(view.sntpSynced);
    unitsDue = true;
    return;
  }
  clusterHandlePing();  // any message from the master is contact
  switch (m.which_body) {
    case wl_ToRow_show_tag: {
      const wl_Show& s = m.body.show;
      if (clusterHandleRender(bootId + connectCount, s.render_id, String(s.text), (int)s.speed,
                              s.commit_at_ms) == ClusterRenderVerdict::Apply) {
        wlClear(out);
        out.which_body = wl_ToMaster_shown_tag;
        out.body.shown.render_id = s.render_id;
        send();
      }
      break;
    }
    case wl_ToRow_quiet_tag:
      clusterNoteLeaderQuiet(m.body.quiet.on);
      break;
    case wl_ToRow_config_tag: {
      // Each setting is written to flash only when it differs from the stored
      // one, so a master may repeat this on every connection.
      const wl_Config& c = m.body.config;
      if (c.fallback <= wl_Fallback_FALLBACK_DATE) {
        prefsStageFallback((FollowerFallback)c.fallback);
      }
      prefsStageReflashOnBoot(c.update_units_at_start);
      clusterSetTz(String(c.tz));
      break;
    }
    case wl_ToRow_op_tag:
      handleOp(m.body.op);
      break;
    case wl_ToRow_update_tag:
      // The latest offer wins; it is taken up by updateTick().
      offer = m.body.update;
      offerPending = true;
      break;
    case wl_ToRow_log_ctl_tag:
      logOn = m.body.log_ctl.on;
      break;
    case wl_ToRow_ping_tag:
      wlClear(out);
      out.which_body = wl_ToMaster_pong_tag;
      send();
      break;
    case wl_ToRow_restart_tag:
      isPendingReboot = true;
      break;
    case wl_ToRow_release_tag:
      clusterHandleLeave();
      drop(F("released by the master"));
      break;
    default:
      break;  // a message this build does not act on yet
  }
}

}  // namespace

void linkLoopTick() {
  if (bootId == 0) bootId = ESP.random() | 1;
  const FollowerClusterView view = clusterViewGet();
  const bool paired = view.phase != ClusterFollowerPhase::Standalone && view.leaderHost.length() > 0;

  if (!paired || WiFi.status() != WL_CONNECTED) {
    if (sock.connected() || welcomed) drop(F("no master to talk to"));
    return;
  }

  if (sock.connected() && (strcmp(dialledId, view.leaderName.c_str()) != 0 ||
                           strcmp(dialledHost, view.leaderHost.c_str()) != 0)) {
    drop(F("paired with another master"));
    return;
  }

  if (!sock.connected()) {
    if (welcomed) drop(F("connection lost"));
    if ((int32_t)(millis() - nextDialMs) < 0) return;
    char host[48];
    if (!followerLinkHostPart(view.leaderHost.c_str(), host, sizeof(host))) return;
    sock.setTimeout(FOLLOWER_LINK_CONNECT_TIMEOUT_MS);
    if (!sock.connect(host, WALL_LINK_PORT)) {
      drop(F("master not reachable"));
      return;
    }
    sock.setNoDelay(true);
    // The core's connected() can stay true on a dead link; TCP keepalive
    // closes it for us (idle 15 s, 3 probes 5 s apart).
    sock.keepAlive(15, 5, 3);
    reader.reset();
    strlcpy(dialledId, view.leaderName.c_str(), sizeof(dialledId));
    strlcpy(dialledHost, view.leaderHost.c_str(), sizeof(dialledHost));
    connectedAtMs = millis();
    sendHello();
    return;
  }

  int avail = sock.available();
  while (avail > 0) {
    uint8_t chunk[64];
    const int got = sock.read(chunk, avail < (int)sizeof(chunk) ? avail : (int)sizeof(chunk));
    if (got <= 0) break;
    avail -= got;
    size_t fed = 0;
    while (fed < (size_t)got) {
      fed += reader.feed(chunk + fed, (size_t)got - fed);
      while (reader.peek() == WlFeed::Message) {
        wl_ToRow m = wl_ToRow_init_zero;
        if (reader.decode(wl_ToRow_fields, &m)) handle(m, view);
        reader.pop();
        if (!sock.connected()) return;
      }
      if (reader.peek() == WlFeed::Bad) {
        drop(F("malformed message"));
        return;
      }
    }
  }

  if (!welcomed) {
    if (followerLinkElapsed(millis(), connectedAtMs, FOLLOWER_LINK_WELCOME_TIMEOUT_MS)) {
      drop(F("no Welcome"));
    }
    return;
  }
  opTick();
  if (!welcomed) return;
  if (unitOpsBusy() != busySent ||
      followerLinkElapsed(millis(), lastStatusMs, FOLLOWER_LINK_STATUS_INTERVAL_MS)) {
    sendStatus(view.sntpSynced);
  }
  if (welcomed) unitsTick();
  if (welcomed) logTick();
  if (welcomed) updateTick(view);
}

FollowerLinkView linkViewGet() {
  FollowerLinkView v;
  v.connected = welcomed;
  v.connects = connectCount;
  v.drops = dropCount;
  return v;
}
