#pragma once
// ClusterWireGuards.h — the member side of the cluster wire (join / render /
// ping / leave), as far as it is decision and not glue: who may claim a row,
// who may drive it, who may release it, the wire-auth state of a keyed
// member, and the health keys its replies carry. An S3 member and an ESP-01
// row answer the same leader, so they must refuse and reply alike. Handlers
// keep their web-library and storage glue. Natively tested by
// test_cluster_wire_guards (Master).

#include <Arduino.h>

#include "ClusterHmac.h"

// The leader's member table is this large, so a member's row index is
// below it.
#define CLUSTER_MAX_MEMBERS 8
#define CLUSTER_HOST_MAX_LEN 40

// Wire strings that get stored and re-served: printable ASCII only, from
// `lowest` up (0x21 = no spaces, for hosts; 0x20 for display names).
inline bool clusterWirePrintable(const String& v, char lowest) {
  for (unsigned int i = 0; i < v.length(); i++) {
    unsigned char c = (unsigned char)v[i];
    if (c < (unsigned char)lowest || c > 0x7E) return false;
  }
  return true;
}

// --- POST /cluster/join -------------------------------------------------------

enum class ClusterJoinCheck : uint8_t {
  Ok = 0,
  RowOutOfRange,
  BadLeaderHost,
  BadLeaderName,
  // Source-IP binding (#313): a join mints a membership pointing display and
  // firmware traffic at leaderHost, so the caller must actually BE leaderHost
  // — the real leader dials from the exact address it puts in the field. A
  // CSRF'd browser or any other LAN host cannot satisfy this.
  CallerIsNotLeaderHost,
};

// `nameMax` is the tree's storage limit for the leader's display name.
inline ClusterJoinCheck clusterJoinValidate(long row, const String& leaderHost,
                                            const String& leaderName,
                                            size_t nameMax,
                                            const String& callerIp) {
  if (row < 0 || row >= CLUSTER_MAX_MEMBERS) {
    return ClusterJoinCheck::RowOutOfRange;
  }
  if (leaderHost.length() == 0 || leaderHost.length() > CLUSTER_HOST_MAX_LEN ||
      !clusterWirePrintable(leaderHost, 0x21)) {
    return ClusterJoinCheck::BadLeaderHost;
  }
  if (leaderName.length() > nameMax || !clusterWirePrintable(leaderName, 0x20)) {
    return ClusterJoinCheck::BadLeaderName;
  }
  if (leaderHost != callerIp) return ClusterJoinCheck::CallerIsNotLeaderHost;
  return ClusterJoinCheck::Ok;
}

inline int clusterJoinCheckStatus(ClusterJoinCheck c) {
  if (c == ClusterJoinCheck::Ok) return 200;
  return c == ClusterJoinCheck::CallerIsNotLeaderHost ? 403 : 400;
}

inline const __FlashStringHelper* clusterJoinCheckMessage(ClusterJoinCheck c) {
  switch (c) {
    case ClusterJoinCheck::RowOutOfRange: return F("Row out of range");
    case ClusterJoinCheck::BadLeaderHost: return F("Invalid leaderHost");
    case ClusterJoinCheck::BadLeaderName: return F("Invalid leaderName");
    case ClusterJoinCheck::CallerIsNotLeaderHost:
      return F("leaderHost must match the caller's address");
    default:
      return F("");
  }
}

// --- render / ping ------------------------------------------------------------

// A leader-wire request to a member that is joined to someone else. Only the
// joined leader drives a row and keeps it alive; a member without a
// membership has no leader to compare against and answers "not clustered".
inline bool clusterCallerIsForeign(const String& joinedLeaderHost,
                                   const String& callerIp) {
  return joinedLeaderHost.length() > 0 && joinedLeaderHost != callerIp;
}

// --- POST /cluster/leave ------------------------------------------------------

// Two legitimate callers: the leader's reconfigure fan-out (server-to-server,
// no Origin) and the board's own "Leave" button (a LAN browser). A keyed
// member takes the leader arm as a valid SIGNATURE, which beats a spoofed IP;
// an unkeyed one as the leader's address. A bare other LAN host is refused —
// that was an any-host force-leave. A member with no leader has nothing to
// protect: leave is idempotent.
inline bool clusterLeaveAllowed(bool keyed, bool signedOk,
                                const String& joinedLeaderHost,
                                const String& callerIp, bool fromLanBrowser) {
  if (fromLanBrowser) return true;
  if (keyed) return signedOk;
  return joinedLeaderHost.length() == 0 || joinedLeaderHost == callerIp;
}

// --- wire-auth state of a member (#313) ----------------------------------------

// The per-member key the leader mints at join, plus the replay mark. A key
// turns enforcement ON; a pre-HMAC leader sends none and the member stays on
// the source-IP binding. Storage is the tree's (NVS on the S3, the membership
// blob on the ESP-01) — this is what is stored and when it must be rewritten.
struct ClusterMemberAuth {
  uint8_t key[CLUSTER_HMAC_KEY_LEN] = {0};
  bool keyed = false;
  uint64_t lastAcceptedTs = 0;   // monotonic replay mark
  uint64_t lastPersistedTs = 0;  // the mark as last written to storage

  // A join's key (empty or malformed = none). Returns true when the key
  // material changed. Fresh material is a fresh signing epoch (a rebooted
  // leader re-mints), so the mark resets — and the caller must persist that
  // reset, or a member reboot right after reloads a stale-high mark and
  // rejects the new leader.
  bool adoptKey(const String& keyHex) {
    uint8_t fresh[CLUSTER_HMAC_KEY_LEN];
    bool valid = keyHex.length() > 0 && clusterKeyFromHex(keyHex, fresh);
    bool changed = valid != keyed ||
                   (valid && memcmp(fresh, key, CLUSTER_HMAC_KEY_LEN) != 0);
    if (valid) memcpy(key, fresh, CLUSTER_HMAC_KEY_LEN);
    keyed = valid;
    if (changed) {
      lastAcceptedTs = 0;
      lastPersistedTs = 0;
    }
    return changed;
  }

  // Leave: the key and its mark go together.
  void drop() {
    keyed = false;
    lastAcceptedTs = 0;
    lastPersistedTs = 0;
  }

  // What storage held at boot.
  void restored(bool keyValid, uint64_t mark) {
    keyed = keyValid;
    lastAcceptedTs = keyValid ? mark : 0;
    lastPersistedTs = lastAcceptedTs;
  }

  // Verifies one signed request and advances the mark. `markDue` = the mark
  // moved far enough that it should be written; the caller calls
  // markPersisted() when it has staged or done that write.
  bool accept(const String& canonicalMsg, uint64_t ts, const String& macHex,
              uint64_t nowEpochMs, bool synced, bool& markDue) {
    markDue = false;
    if (!keyed) return false;
    bool ok = clusterHmacAccept(key, canonicalMsg, ts, macHex, nowEpochMs,
                                synced, lastAcceptedTs);
    markDue = ok && clusterHmacMarkNeedsPersist(lastAcceptedTs, lastPersistedTs);
    return ok;
  }

  void markPersisted() { lastPersistedTs = lastAcceptedTs; }
};

// --- the health keys of a join / ping reply (#294) -----------------------------

struct ClusterRowHealth {
  int width = 0;
  int detected = 0;
  int faulty = 0;
  const char* faultMask = "";
  int lost = 0;          // #497: stale sketch units
  bool busDead = false;  // #497: row-wide I2C bus death (#488)
  bool wear = false;
};

// ,"width":W,"detected":D,"faulty":F,"faultMask":"..","lost":L[,"busDead":1],
// "wear":B — the same block in the join and the ping reply of every member,
// so the leader's strip is live from the handshake.
inline void clusterAppendHealthKeys(String& out, const ClusterRowHealth& h) {
  out += F(",\"width\":");
  out += h.width;
  out += F(",\"detected\":");
  out += h.detected;
  out += F(",\"faulty\":");
  out += h.faulty;
  out += F(",\"faultMask\":\"");
  out += h.faultMask;
  out += F("\",\"lost\":");
  out += h.lost;
  // Additive, int so the leader's bare-number extractor reads it; absent =
  // bus alive, so older leaders see an unchanged reply.
  if (h.busDead) out += F(",\"busDead\":1");
  out += F(",\"wear\":");
  out += h.wear ? F("true") : F("false");
}
