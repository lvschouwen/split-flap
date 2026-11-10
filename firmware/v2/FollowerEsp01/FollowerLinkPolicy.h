#pragma once
// FollowerLinkPolicy.h — the timing rules of the row board's wall link
// (#559/#564) and what it tells the TX ladder about the row's uplink (#668),
// pure and natively tested by test_follower_link. The socket
// glue lives in FollowerLink.cpp.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "WifiTxPolicy.h"

// Redial delay after a failed or dropped connection: 1 s, doubling to 8 s.
#define FOLLOWER_LINK_BACKOFF_MIN_MS 1000UL
#define FOLLOWER_LINK_BACKOFF_MAX_MS 8000UL
// A connect attempt blocks loop() for at most this long.
#define FOLLOWER_LINK_CONNECT_TIMEOUT_MS 1000UL
// Vitals go up this often while connected.
#define FOLLOWER_LINK_STATUS_INTERVAL_MS 10000UL
// The unit facts go up this often while connected, after every job and as
// soon as a render has settled (#640) — but a settled render waits until this
// long after the last document, so a burst of renders sends one per gap.
#define FOLLOWER_LINK_UNITS_INTERVAL_MS 30000UL
#define FOLLOWER_LINK_UNITS_MIN_GAP_MS 2000UL
// A master that has not answered Hello within this is not a master.
#define FOLLOWER_LINK_WELCOME_TIMEOUT_MS 5000UL

// A write waits this long for the master to acknowledge what is in the way.
// A segment lost on the air comes back with its retransmission, a second or
// more later: a shorter wait closes the connection over one lost frame.
#define FOLLOWER_LINK_WRITE_TIMEOUT_MS 5000UL
// What can wait leaves this much of the send buffer free, so an answer the
// master waits for (Shown, Pong, a job's state) never finds it full.
#define FOLLOWER_LINK_REPLY_RESERVE 128U
// The master writes at least every 10 s (its Ping) to a row that is not busy;
// nothing from it for this long means it is gone or cannot reach this row,
// and the row dials again.
#define FOLLOWER_LINK_SILENT_MS 30000UL
// A pass of the link this late means loop() stood still.
#define FOLLOWER_LINK_STALL_MS 2000UL
// The master pings every 10 s: heard within this, its messages still reach
// this row, so a connection that ends now ended on the row's own direction.
#define FOLLOWER_LINK_HEARD_RECENT_MS 15000UL

inline uint32_t followerLinkNextBackoffMs(uint32_t previousMs) {
  if (previousMs < FOLLOWER_LINK_BACKOFF_MIN_MS) return FOLLOWER_LINK_BACKOFF_MIN_MS;
  const uint32_t next = previousMs * 2;
  return next > FOLLOWER_LINK_BACKOFF_MAX_MS ? FOLLOWER_LINK_BACKOFF_MAX_MS : next;
}

// millis()-wrap-safe "has `intervalMs` passed since `sinceMs`".
inline bool followerLinkElapsed(uint32_t nowMs, uint32_t sinceMs, uint32_t intervalMs) {
  return (uint32_t)(nowMs - sinceMs) >= intervalMs;
}

// Is a new unit facts document due? `renderSeq` counts the renders that have
// settled (FollowerBus.h busRendersSettled), `sentRenderSeq` is that count
// when the last document was started.
inline bool followerLinkUnitsDue(uint32_t nowMs, uint32_t lastUnitsMs, bool jobDue,
                                 uint32_t renderSeq, uint32_t sentRenderSeq) {
  if (jobDue || followerLinkElapsed(nowMs, lastUnitsMs, FOLLOWER_LINK_UNITS_INTERVAL_MS)) {
    return true;
  }
  return renderSeq != sentRenderSeq &&
         followerLinkElapsed(nowMs, lastUnitsMs, FOLLOWER_LINK_UNITS_MIN_GAP_MS);
}

// May a message that can wait (vitals, unit facts, a log line, an event) be
// written now? Only when the send buffer takes it whole: such a write never
// waits for the master, so late acknowledgements cost nothing but time.
inline bool followerLinkRoomFor(size_t freeBytes, size_t messageBytes) {
  return freeBytes >= messageBytes + FOLLOWER_LINK_REPLY_RESERVE;
}

inline bool followerLinkMasterSilent(uint32_t nowMs, uint32_t heardMs) {
  return followerLinkElapsed(nowMs, heardMs, FOLLOWER_LINK_SILENT_MS);
}

// The instant the master counts as last heard, after a pass of the link.
// The count starts again while this row is busy with a unit job (the master
// does not ping a busy row), and after a late pass: while loop() stood still
// (a unit update, a download) what the master sent waits unread.
inline uint32_t followerLinkHeardAfterPass(uint32_t heardMs, uint32_t lastPassMs, uint32_t nowMs,
                                           bool busy) {
  if (busy || followerLinkElapsed(nowMs, lastPassMs, FOLLOWER_LINK_STALL_MS)) return nowMs;
  return heardMs;
}

// A stored master address may carry a port ("host:8801", a record written
// by an image before the link); the link always dials its own port, so only
// the host part counts.
// Copies it into out (cap bytes); false when there is none or it does not fit.
inline bool followerLinkHostPart(const char* stored, char* out, size_t cap) {
  const char* colon = strchr(stored, ':');
  const size_t n = colon ? (size_t)(colon - stored) : strlen(stored);
  if (n == 0 || n >= cap) return false;
  memcpy(out, stored, n);
  out[n] = 0;
  return true;
}

// A row paired with one master must not take orders from another: the name a
// master gives in Welcome has to be the one stored at pairing.
inline bool followerLinkMasterAccepted(const char* pairedName, const char* welcomeName) {
  return pairedName[0] != 0 && strcmp(pairedName, welcomeName) == 0;
}

// What ended a connection, as far as the TX ladder cares.
enum class FollowerLinkClose : uint8_t {
  Plain,        // the row or the master chose to end it, or there is no master to reach
  NoWelcome,    // the master did not answer Hello in time
  Lost,         // the connection ended under a welcomed row
  WriteFailed,  // an answer waited its whole timeout for room
};

// Did the row's uplink demonstrably fail? That is a link drop for the TX
// ladder, like a lost association. `unackedBytes` < 0 = no send buffer to read.
// A Hello still unacknowledged when the Welcome wait ends never reached the
// master; a connection lost or a write stuck while the master's messages
// still arrive is the master not hearing this row (its own silence rule).
inline bool followerLinkUplinkFailed(FollowerLinkClose kind, int unackedBytes, bool welcomed,
                                     uint32_t heardAgoMs) {
  switch (kind) {
    case FollowerLinkClose::NoWelcome:
      return unackedBytes > 0;
    case FollowerLinkClose::Lost:
    case FollowerLinkClose::WriteFailed:
      return welcomed && heardAgoMs < FOLLOWER_LINK_HEARD_RECENT_MS;
    case FollowerLinkClose::Plain:
      break;
  }
  return false;
}

// A failed uplink is owed one level up, paid by the Online step under the
// ladder's own rules (settle, dwell, idle units; the level kept for the boot).
// At the online ceiling it has nothing to buy, and left pending it would hold
// the step-down off. True when the step is owed.
inline bool followerLinkOweTxStep(WifiTxState& st) {
  if (st.index >= WIFI_TX_ONLINE_CEILING_INDEX) return false;
  st.dropPending = true;
  return true;
}

// "Traffic confirmed" for the TX ladder: the master acknowledged everything
// the row wrote. Downlink messages prove nothing about the row's own frames.
struct FollowerUplinkAck {
  bool written = false;  // bytes went out that are not yet seen acknowledged
  uint32_t ackedMs = 0;  // when the send buffer was last seen drained; 0 = never
};

inline void followerLinkNoteWrite(FollowerUplinkAck& a) { a.written = true; }

// A closed connection takes its unacknowledged bytes with it.
inline void followerLinkNoteClosed(FollowerUplinkAck& a) { a.written = false; }

// Called with the send buffer's free bytes on an open connection.
inline void followerLinkNoteSendBuffer(FollowerUplinkAck& a, size_t freeBytes, size_t bufBytes,
                                       uint32_t nowMs) {
  if (!a.written || freeBytes < bufBytes) return;
  a.written = false;
  a.ackedMs = nowMs != 0 ? nowMs : 1;
}

inline bool followerLinkUplinkConfirmed(const FollowerUplinkAck& a, uint32_t nowMs) {
  return wifiTxTrafficFresh(a.ackedMs, nowMs);
}
