#pragma once
// ClusterFollowerPolicy.h — pure follower-side cluster state machine
// (#272, epic #270; spec docs/superpowers/specs/
// 2026-07-13-multi-display-cluster-design.md), natively tested by
// test_cluster_follower_policy. No networking, no NVS — WebEndpoints /
// netTask glue feeds events in and executes what the phase implies.
//
// Phases: Standalone (not a cluster member) → Clustered (leader feeding)
// → Grace (leader silent, HOLD the last segment) → LocalFallback (leader
// gone ~2 min, show own clock). Any leader contact reclaims straight to
// Clustered. A reboot with `clusteredBy` persisted boots into Grace —
// never flashes stale standalone content.
//
// Staleness armor: `epoch` is a random ID minted at leader boot, `seq`
// monotonic within it. Same-epoch renders with seq <= lastSeq are
// duplicates (delayed retries can't regress the wall) but still count as
// leader contact; ANY new epoch is accepted (leader rebooted). The
// producer gate (web text/mode/clock 409, clockTask stand-down, MQTT
// availability-only) holds in every phase except Standalone; maintenance
// stays local always.

#include <stdint.h>

// Leader pings ~10 s when idle; ~2.5 missed pings parks Clustered into
// Grace, and the spec's ~2 min of total silence drops Grace into
// LocalFallback.
static const uint32_t CLUSTER_CONTACT_FRESH_MS = 25000UL;
static const uint32_t CLUSTER_GRACE_MS = 120000UL;

// A commitAtMs further out than this is a bogus timestamp, not a
// synchronized flip — clamp instead of parking the display.
static const uint32_t CLUSTER_COMMIT_MAX_DELAY_MS = 5000UL;

enum class ClusterFollowerPhase : uint8_t {
  Standalone = 0,
  Clustered,
  Grace,
  LocalFallback,
};

enum class ClusterRenderVerdict : uint8_t {
  Apply = 0,     // fresh: enqueue the segment
  Duplicate,     // stale epoch/seq pair: contact only, don't re-render
  NotClustered,  // no membership: leader must POST /cluster/join first
};

struct ClusterFollowerState {
  ClusterFollowerPhase phase = ClusterFollowerPhase::Standalone;
  uint32_t epoch = 0;
  bool haveEpoch = false;
  uint32_t lastSeq = 0;
  uint32_t lastContactMs = 0;
};

// Boot entry: with a persisted membership the follower comes up gated in
// Grace (the leader will re-join/re-render); without one, Standalone.
inline void clusterFollowerBoot(ClusterFollowerState& st, uint32_t nowMs,
                                bool membershipStored) {
  st = ClusterFollowerState{};
  if (membershipStored) {
    st.phase = ClusterFollowerPhase::Grace;
    st.lastContactMs = nowMs;  // the grace window runs from boot
  }
}

// Accepted POST /cluster/join. Seq tracking resets ONLY on a new epoch: a
// same-epoch re-join (leader recovering a degraded member, no reboot) must
// keep rejecting delayed retries of old renders — the leader mints fresh,
// higher seqs, so its post-rejoin re-send still applies.
inline void clusterFollowerJoin(ClusterFollowerState& st, uint32_t nowMs,
                                uint32_t epoch) {
  st.phase = ClusterFollowerPhase::Clustered;
  if (!st.haveEpoch || epoch != st.epoch) {
    st.epoch = epoch;
    st.haveEpoch = true;
    st.lastSeq = 0;
  }
  st.lastContactMs = nowMs;
}

// Any authenticated-by-membership leader contact (ping, health poll…).
// Returns false in Standalone — the reply tells the leader to re-join.
inline bool clusterFollowerContact(ClusterFollowerState& st, uint32_t nowMs) {
  if (st.phase == ClusterFollowerPhase::Standalone) return false;
  st.phase = ClusterFollowerPhase::Clustered;  // reclaim is seamless
  st.lastContactMs = nowMs;
  return true;
}

// POST /cluster/render acceptance. Duplicates still feed the grace timer.
inline ClusterRenderVerdict clusterFollowerAcceptRender(
    ClusterFollowerState& st, uint32_t nowMs, uint32_t epoch, uint32_t seq) {
  if (st.phase == ClusterFollowerPhase::Standalone) {
    return ClusterRenderVerdict::NotClustered;
  }
  clusterFollowerContact(st, nowMs);
  if (st.haveEpoch && epoch == st.epoch && seq <= st.lastSeq) {
    return ClusterRenderVerdict::Duplicate;
  }
  st.epoch = epoch;  // adopts any NEW epoch: the leader rebooted
  st.haveEpoch = true;
  st.lastSeq = seq;
  return ClusterRenderVerdict::Apply;
}

// POST /cluster/leave (or local uncluster): back to Standalone.
inline void clusterFollowerLeave(ClusterFollowerState& st) {
  st = ClusterFollowerState{};
}

// 1 Hz-ish supervision: Clustered decays to Grace, Grace to LocalFallback.
// Returns true when the phase changed (glue logs the transition). Rollover
// safe: uint32 elapsed stays valid while real silence is under 2^31 ms.
inline bool clusterFollowerTick(ClusterFollowerState& st, uint32_t nowMs) {
  uint32_t silence = nowMs - st.lastContactMs;
  bool changed = false;
  if (st.phase == ClusterFollowerPhase::Clustered &&
      silence >= CLUSTER_CONTACT_FRESH_MS) {
    st.phase = ClusterFollowerPhase::Grace;
    changed = true;
  }
  // Cascades: the phase tracks total silence, not tick cadence — a starved
  // tick can't stretch the grace window.
  if (st.phase == ClusterFollowerPhase::Grace && silence >= CLUSTER_GRACE_MS) {
    st.phase = ClusterFollowerPhase::LocalFallback;
    changed = true;
  }
  return changed;
}

// Producer gate: every display-content producer (web text/mode/clock,
// clockTask, MQTT commands) stands down while a cluster membership exists.
inline bool clusterFollowerGatesProducers(const ClusterFollowerState& st) {
  return st.phase != ClusterFollowerPhase::Standalone;
}

// LocalFallback shows the follower's OWN clock regardless of deviceMode.
inline bool clusterFollowerForcesLocalClock(const ClusterFollowerState& st) {
  return st.phase == ClusterFollowerPhase::LocalFallback;
}

// #295 sticky leadership: a join from a DIFFERENT leader is rejected only
// while the current one is demonstrably alive (Clustered with fresh
// contact). Grace/LocalFallback mean the leader has gone silent — a
// promoted successor may claim the follower; the same leader always may.
inline bool clusterFollowerJoinConflicts(const ClusterFollowerState& st,
                                         uint32_t nowMs, bool sameLeader) {
  if (sameLeader) return false;
  if (st.phase != ClusterFollowerPhase::Clustered) return false;
  return nowMs - st.lastContactMs < CLUSTER_CONTACT_FRESH_MS;
}

// #295 promote gate: only a follower that has fully written the leader off
// (LocalFallback) may take over — Grace still expects the leader back.
inline bool clusterFollowerCanPromote(const ClusterFollowerState& st) {
  return st.phase == ClusterFollowerPhase::LocalFallback;
}

// Synchronized flip: ms to wait before enqueueing a render. Unsynced
// clocks render immediately (spec), past timestamps too; far-future ones
// clamp to CLUSTER_COMMIT_MAX_DELAY_MS.
inline uint32_t clusterRenderDelayMs(uint64_t commitAtMs, uint64_t nowEpochMs,
                                     bool timeSynced) {
  if (!timeSynced || commitAtMs <= nowEpochMs) return 0;
  uint64_t delay = commitAtMs - nowEpochMs;
  if (delay > CLUSTER_COMMIT_MAX_DELAY_MS) return CLUSTER_COMMIT_MAX_DELAY_MS;
  return (uint32_t)delay;
}

// /cluster/health state vocabulary.
inline const char* clusterFollowerPhaseName(ClusterFollowerPhase p) {
  switch (p) {
    case ClusterFollowerPhase::Clustered:     return "clustered";
    case ClusterFollowerPhase::Grace:         return "grace";
    case ClusterFollowerPhase::LocalFallback: return "local-fallback";
    default:                                  return "standalone";
  }
}
