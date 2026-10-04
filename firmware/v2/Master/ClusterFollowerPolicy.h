#pragma once
// ClusterFollowerPolicy.h — what an S3 member adds to the shared phase
// machine (ClusterMemberPhase.h, #272): the producer gate, the local-clock
// fallback once the leader is written off, and the #321 ranked auto-takeover.
// Natively tested by test_cluster_follower_policy. No networking, no NVS.
//
// The producer gate (web text/mode/clock 409, clockTask stand-down, MQTT
// availability-only) holds in every phase except Standalone; maintenance
// stays local always.

#include <stdint.h>

#include "ClusterMemberPhase.h"

// Ranked auto-takeover (#321). A designated-successor S3 self-promotes once the
// leader has been silent this long — 30 s, deliberately EARLIER than the 120 s
// LeaderLost manual-promote gate, so the backup claims the wall before the
// ESP-01 rows blank. Lower-ranked successors wait an extra STAGGER each, so the
// primary (rank 0) always fires first; if it takes over, its re-join clears
// everyone's silence and the lower ranks never fire (deterministic, no votes).
static const uint32_t CLUSTER_AUTO_TAKEOVER_MS = 30000UL;
static const uint32_t CLUSTER_AUTO_TAKEOVER_STAGGER_MS = 5000UL;
// Cooldown after a FAILED auto-promote so it can't re-fire every service tick.
static const uint32_t CLUSTER_AUTO_TAKEOVER_RETRY_MS = 5000UL;
// Cooldown after a SUCCESSFUL auto-promote. If it was a false positive (a
// wedged/flapping follower promoted, then the still-alive leader sticky-demotes
// it), this stops an immediate re-promote — breaking the promote<->demote churn
// so the board rejoins and settles instead of thrashing the wall.
static const uint32_t CLUSTER_AUTO_TAKEOVER_COOLDOWN_MS = 120000UL;
// A planned leader restart (OTA/reboot/config) announces a hold so nobody takes
// over during the reboot window; clamped so a bogus value can't wedge takeover.
static const uint32_t CLUSTER_AUTO_TAKEOVER_HOLD_MAX_MS = 60000UL;

// Producer gate: every display-content producer (web text/mode/clock,
// clockTask, MQTT commands) stands down while a cluster membership exists.
inline bool clusterFollowerGatesProducers(const ClusterFollowerState& st) {
  return st.phase != ClusterFollowerPhase::Standalone;
}

// A member that wrote its leader off shows its OWN clock regardless of
// deviceMode.
inline bool clusterFollowerForcesLocalClock(const ClusterFollowerState& st) {
  return st.phase == ClusterFollowerPhase::LeaderLost;
}

// #295 promote gate: only a follower that has fully written the leader off
// (LeaderLost) may take over — Grace still expects the leader back.
inline bool clusterFollowerCanPromote(const ClusterFollowerState& st) {
  return st.phase == ClusterFollowerPhase::LeaderLost;
}

// #321: this follower's rank among the leader's ordered eligible-successor list
// (member indices, e.g. "2,3"), or -1 if it is not a designated successor (an
// ESP-01, a foreign-plat board, or absent — the leader excludes those). rank 0
// is the primary successor.
inline int clusterSuccessorRank(const char* succCsv, int selfIndex) {
  if (selfIndex < 0 || succCsv == nullptr) return -1;
  int rank = 0;
  const char* p = succCsv;
  while (*p) {
    if (*p < '0' || *p > '9') {  // comma / any separator
      p++;
      continue;
    }
    long v = 0;
    while (*p >= '0' && *p <= '9') {
      v = v * 10 + (*p - '0');
      p++;
    }
    if (v == selfIndex) return rank;
    rank++;
  }
  return -1;
}

// #321 ranked auto-takeover trigger. A designated successor (rank >= 0)
// self-promotes once leader silence reaches the base delay plus its rank
// stagger — UNLESS a graceful-reboot hold is still suppressing takeover, or
// the leader is still fresh (Standalone/Clustered). Independent of the manual
// LeaderLost gate: this fires during Grace, before the wall goes dark.
inline bool clusterFollowerAutoPromoteDue(const ClusterFollowerState& st,
                                          int successorRank,
                                          uint32_t holdUntilMs, uint32_t nowMs) {
  if (successorRank < 0) return false;
  if (st.phase == ClusterFollowerPhase::Standalone ||
      st.phase == ClusterFollowerPhase::Clustered) {
    return false;  // no membership, or the leader is still in contact
  }
  if ((int32_t)(nowMs - holdUntilMs) < 0) return false;  // hold still active
  uint32_t threshold = CLUSTER_AUTO_TAKEOVER_MS +
                       (uint32_t)successorRank * CLUSTER_AUTO_TAKEOVER_STAGGER_MS;
  return (nowMs - st.lastContactMs) >= threshold;
}

// #321: absolute deadline until which auto-takeover is suppressed, from a
// leader's announced hold. Clamped so a hostile/bogus hold can't wedge it.
inline uint32_t clusterAutoTakeoverHoldDeadline(uint32_t holdMs,
                                                uint32_t nowMs) {
  if (holdMs > CLUSTER_AUTO_TAKEOVER_HOLD_MAX_MS) {
    holdMs = CLUSTER_AUTO_TAKEOVER_HOLD_MAX_MS;
  }
  return nowMs + holdMs;
}

// /cluster/health state vocabulary: an S3 that wrote its leader off shows its
// own clock.
inline const char* clusterFollowerPhaseName(ClusterFollowerPhase p) {
  return clusterFollowerPhaseName(p, "local-fallback");
}
