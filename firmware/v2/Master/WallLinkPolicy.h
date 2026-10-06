#pragma once
// WallLinkPolicy.h — the master's rules for one row board's connection on the
// wall link (#559/#566), pure and natively tested by test_wall_link_policy.
// The sockets live in the link task; the messages are
// firmware/v2/link/wall_link.proto.
//
// A row board in a unit job does not read its socket, and every message left
// unread costs it memory. So nothing here queues: the master keeps the latest
// text per row, pings only a connection it has not written to for a while, and
// holds both while the row reports itself busy.

#include <stdint.h>
#include <string.h>

#include "WallLinkStream.h"

// The master writes a Ping to a connection it has not written to for this long.
#define WALL_LINK_PING_IDLE_MS 10000UL
// A row not heard from for this long is lost (#385).
#define WALL_LINK_LOST_MS 30000UL
// A connection that has not said Hello within this is not a row board.
#define WALL_LINK_HELLO_TIMEOUT_MS 5000UL
// TCP keepalive on a row's connection: a busy row is silent on the link but
// its TCP stack still answers, so this is what finds a busy row without
// power. Idle + interval * count is the lost mark.
#define WALL_LINK_KEEPALIVE_IDLE_S 10UL
#define WALL_LINK_KEEPALIVE_INTERVAL_S 5UL
#define WALL_LINK_KEEPALIVE_COUNT 4UL

// The longest text one row takes: Show.text without its terminator.
#define WALL_ROW_TEXT_MAX (sizeof(((wl_Show*)0)->text) - 1)

// millis()-wrap-safe "has `intervalMs` passed since `sinceMs`".
inline bool wallLinkElapsed(uint32_t nowMs, uint32_t sinceMs, uint32_t intervalMs) {
  return (uint32_t)(nowMs - sinceMs) >= intervalMs;
}

// ---- contact -------------------------------------------------------------------

// What the master knows about reaching one row. Outlives a connection: the
// last time the row was heard and its boot id carry over a redial.
struct WallRowContact {
  bool connected = false;
  bool helloSeen = false;   // on the current connection
  bool busy = false;        // the row's last Status said a unit job holds it
  bool everHeard = false;
  bool haveBootId = false;
  uint32_t connectedAtMs = 0;
  uint32_t lastHeardMs = 0;
  uint32_t lastSentMs = 0;
  uint32_t bootId = 0;
};

inline void wallRowConnected(WallRowContact& c, uint32_t nowMs) {
  c.connected = true;
  c.helloSeen = false;
  c.busy = false;
  c.connectedAtMs = nowMs;
  c.lastSentMs = nowMs;
}

inline void wallRowDropped(WallRowContact& c) {
  c.connected = false;
  c.helloSeen = false;
  c.busy = false;
}

// Any message from the row counts as contact.
inline void wallRowHeard(WallRowContact& c, uint32_t nowMs) {
  c.everHeard = true;
  c.lastHeardMs = nowMs;
}

inline void wallRowSent(WallRowContact& c, uint32_t nowMs) { c.lastSentMs = nowMs; }

inline bool wallLinkPingDue(const WallRowContact& c, uint32_t nowMs) {
  return c.connected && c.helloSeen && !c.busy &&
         wallLinkElapsed(nowMs, c.lastSentMs, WALL_LINK_PING_IDLE_MS);
}

inline bool wallLinkHelloOverdue(const WallRowContact& c, uint32_t nowMs) {
  return c.connected && !c.helloSeen &&
         wallLinkElapsed(nowMs, c.connectedAtMs, WALL_LINK_HELLO_TIMEOUT_MS);
}

enum class WallHello : uint8_t { Accept, WrongProtocol, NoId, NotPaired };

// `known`: the id is in this master's rows table.
inline WallHello wallLinkJudgeHello(uint32_t protocol, const char* id, bool known) {
  if (protocol != WALL_LINK_PROTOCOL) return WallHello::WrongProtocol;
  if (id[0] == 0) return WallHello::NoId;
  return known ? WallHello::Accept : WallHello::NotPaired;
}

enum class WallRowReach : uint8_t {
  Never,  // no message from it since this master started
  Up,
  Busy,   // connected, in a unit job: silence is expected
  Away,   // no connection, heard less than the lost mark ago
  Lost,
};

inline WallRowReach wallRowReach(const WallRowContact& c, uint32_t nowMs) {
  if (!c.everHeard) return WallRowReach::Never;
  if (c.connected && c.busy) return WallRowReach::Busy;
  if (wallLinkElapsed(nowMs, c.lastHeardMs, WALL_LINK_LOST_MS)) return WallRowReach::Lost;
  return c.connected ? WallRowReach::Up : WallRowReach::Away;
}

// Records the boot id of a Hello; true when the row has restarted since the
// last one, which fails every job the master still holds open on it.
inline bool wallRowNoteBoot(WallRowContact& c, uint32_t bootId) {
  const bool restarted = c.haveBootId && c.bootId != bootId;
  c.haveBootId = true;
  c.bootId = bootId;
  return restarted;
}

// ---- text: the latest only -----------------------------------------------------

struct WallRowText {
  char text[WALL_ROW_TEXT_MAX + 1] = {0};
  uint64_t commitAtMs = 0;  // flip instant, Unix ms; 0 = on arrival
  uint32_t renderId = 0;    // 0 = nothing was ever shown on this row
  uint32_t shownId = 0;     // the last render the row reported applied
  uint16_t speed = 0;
  bool pending = false;     // not yet written to the connection
};

// Replaces whatever was waiting. False when the row already has (or is about
// to get) exactly this text at this speed.
inline bool wallRowTextSet(WallRowText& t, const char* text, uint16_t speed, uint64_t commitAtMs) {
  char cut[sizeof(t.text)];
  strncpy(cut, text, sizeof(cut) - 1);
  cut[sizeof(cut) - 1] = 0;
  if (t.renderId != 0 && t.speed == speed && strcmp(t.text, cut) == 0) return false;
  memcpy(t.text, cut, sizeof(cut));
  t.speed = speed;
  t.commitAtMs = commitAtMs;
  t.renderId++;
  t.pending = true;
  return true;
}

// For a new connection: the row may have fallen back or restarted blank, so
// the current text goes out again, to flip on arrival.
inline void wallRowTextResend(WallRowText& t) {
  if (t.renderId == 0) return;
  t.commitAtMs = 0;
  t.renderId++;
  t.pending = true;
}

inline bool wallRowTextDue(const WallRowText& t, const WallRowContact& c) {
  return t.pending && c.connected && c.helloSeen && !c.busy;
}

inline void wallRowTextSent(WallRowText& t) { t.pending = false; }
inline void wallRowTextShown(WallRowText& t, uint32_t renderId) { t.shownId = renderId; }
inline bool wallRowTextApplied(const WallRowText& t) {
  return !t.pending && t.renderId != 0 && t.shownId == t.renderId;
}

// ---- a unit job: one at a time per row -------------------------------------------

// A job the row was not handed within this is given up. The master never
// wrote it, so the row cannot be running it.
#define WALL_JOB_HAND_OVER_MS 20000UL
// A job on one unit: four times the longest measured (a self-test, 29 s).
#define WALL_JOB_UNIT_MS 120000UL
// Updating every unit of a row: a minute for each (12.3 s measured) of the 16
// a row can hold.
#define WALL_JOB_ALL_UNITS_MS 960000UL

enum class WallJobStage : uint8_t { None, Waiting, Sent };
enum class WallJobEnd : uint8_t { NotHandedOver, NoResult };

// The job the master holds open on a row. The row answers every Op it reads
// with OpState; the master names the job (op.op_id) and the row echoes it.
struct WallRowJob {
  WallJobStage stage = WallJobStage::None;
  uint32_t sinceMs = 0;  // Waiting: when it was asked for; Sent: when it was written
  wl_Op op = wl_Op_init_zero;
};

inline uint32_t wallJobRunMs(const wl_Op& op) {
  const bool everyUnit = op.opcode == wl_OpCode_OPC_UPDATE_UNITS && op.address == 0;
  return everyUnit ? WALL_JOB_ALL_UNITS_MS : WALL_JOB_UNIT_MS;
}

// False when the row already has a job.
inline bool wallJobStart(WallRowJob& j, const wl_Op& op, uint32_t nowMs) {
  if (j.stage != WallJobStage::None) return false;
  j.stage = WallJobStage::Waiting;
  j.sinceMs = nowMs;
  j.op = op;
  return true;
}

inline bool wallJobDue(const WallRowJob& j, const WallRowContact& c) {
  return j.stage == WallJobStage::Waiting && c.connected && c.helloSeen && !c.busy;
}

inline void wallJobSent(WallRowJob& j, uint32_t nowMs) {
  j.stage = WallJobStage::Sent;
  j.sinceMs = nowMs;
}

// A written job is waited for through a dropped connection: the row reports
// the end again on the next one. Only time ends it without an answer.
inline bool wallJobOverdue(const WallRowJob& j, uint32_t nowMs, WallJobEnd& why) {
  if (j.stage == WallJobStage::Waiting && wallLinkElapsed(nowMs, j.sinceMs, WALL_JOB_HAND_OVER_MS)) {
    why = WallJobEnd::NotHandedOver;
    return true;
  }
  if (j.stage == WallJobStage::Sent && wallLinkElapsed(nowMs, j.sinceMs, wallJobRunMs(j.op))) {
    why = WallJobEnd::NoResult;
    return true;
  }
  return false;
}

// Is this OpState about the row's open job? An ending one closes it. Anything
// else is the echo of a job the master has already given up.
inline bool wallJobAnswer(WallRowJob& j, const wl_OpState& state) {
  if (j.stage != WallJobStage::Sent || state.op_id != j.op.op_id) return false;
  if (state.phase != wl_OpPhase_OP_RUNNING) j = WallRowJob{};
  return true;
}
