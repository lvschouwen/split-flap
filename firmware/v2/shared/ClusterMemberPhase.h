#pragma once
// ClusterMemberPhase.h — the phase machine every cluster member runs under a
// leader (#272, epic #270), S3 and ESP-01 alike. Pure: the tree's web and
// service glue feeds events in and executes what the phase implies. Natively
// tested by test_cluster_follower_policy (Master) and test_follower_policy
// (FollowerEsp01).
//
// Phases: Standalone (not a member) → Clustered (leader feeding) → Grace
// (leader silent, HOLD the last segment) → LeaderLost (silent ~2 min). What
// LeaderLost shows is the tree's choice — an S3 falls back to its own clock,
// an ESP-01 row blanks. Any leader contact reclaims straight to Clustered. A
// reboot with a persisted membership boots into Grace, never into stale
// standalone content.
//
// Staleness armor: `epoch` is a random ID minted at leader boot, `seq`
// monotonic within it. Same-epoch renders with seq <= lastSeq are duplicates
// (delayed retries can't regress the wall) but still count as leader contact;
// ANY new epoch is accepted (leader rebooted).

#include <stdint.h>

// Leader pings ~10 s when idle; ~2.5 missed pings parks Clustered into
// Grace, and ~2 min of total silence writes the leader off. One set of
// values so the wall degrades uniformly.
static const uint32_t CLUSTER_CONTACT_FRESH_MS = 25000UL;
static const uint32_t CLUSTER_GRACE_MS = 120000UL;

// A commitAtMs further out than this is a bogus timestamp, not a
// synchronized flip — clamp instead of parking the display.
static const uint32_t CLUSTER_COMMIT_MAX_DELAY_MS = 5000UL;

enum class ClusterFollowerPhase : uint8_t {
  Standalone = 0,
  Clustered,
  Grace,
  LeaderLost,
};

enum class ClusterRenderVerdict : uint8_t {
  Apply = 0,     // fresh: render the segment
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

// Boot entry: with a persisted membership the member comes up gated in
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

// Any authenticated-by-membership leader contact (ping, render…). Returns
// false in Standalone — the reply tells the leader to re-join.
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

// The leader is demonstrably reaching us and getting answers. Boot stamps
// lastContactMs for the grace window, so the phase matters: Grace is a
// stored membership, not a contact.
inline bool clusterFollowerContactFresh(const ClusterFollowerState& st,
                                        uint32_t nowMs) {
  return st.phase == ClusterFollowerPhase::Clustered &&
         nowMs - st.lastContactMs < CLUSTER_CONTACT_FRESH_MS;
}

// ~1 Hz supervision: Clustered decays to Grace, Grace to LeaderLost. Returns
// true when the phase changed (glue logs the transition). Cascades on total
// silence, not tick cadence — a starved tick can't stretch the grace window.
// Rollover safe: uint32 elapsed stays valid while real silence is under
// 2^31 ms.
inline bool clusterFollowerTick(ClusterFollowerState& st, uint32_t nowMs) {
  uint32_t silence = nowMs - st.lastContactMs;
  bool changed = false;
  if (st.phase == ClusterFollowerPhase::Clustered &&
      silence >= CLUSTER_CONTACT_FRESH_MS) {
    st.phase = ClusterFollowerPhase::Grace;
    changed = true;
  }
  if (st.phase == ClusterFollowerPhase::Grace && silence >= CLUSTER_GRACE_MS) {
    st.phase = ClusterFollowerPhase::LeaderLost;
    changed = true;
  }
  return changed;
}

// #295 sticky leadership: a join from a DIFFERENT leader is rejected only
// while the current one is demonstrably alive (Clustered with fresh
// contact). Grace/LeaderLost mean the leader has gone silent — a promoted
// successor may claim the member; the same leader always may.
inline bool clusterFollowerJoinConflicts(const ClusterFollowerState& st,
                                         uint32_t nowMs, bool sameLeader) {
  if (sameLeader) return false;
  return clusterFollowerContactFresh(st, nowMs);
}

// Synchronized flip: ms to wait before rendering. Unsynced clocks render
// immediately, past timestamps too; far-future ones clamp to
// CLUSTER_COMMIT_MAX_DELAY_MS.
inline uint32_t clusterRenderDelayMs(uint64_t commitAtMs, uint64_t nowEpochMs,
                                     bool timeSynced) {
  if (!timeSynced || commitAtMs <= nowEpochMs) return 0;
  uint64_t delay = commitAtMs - nowEpochMs;
  if (delay > CLUSTER_COMMIT_MAX_DELAY_MS) return CLUSTER_COMMIT_MAX_DELAY_MS;
  return (uint32_t)delay;
}

// /cluster/health state vocabulary. The tree names its own LeaderLost
// behaviour ("local-fallback" on an S3, "blank" on an ESP-01 row).
inline const char* clusterFollowerPhaseName(ClusterFollowerPhase p,
                                            const char* leaderLostName) {
  switch (p) {
    case ClusterFollowerPhase::Clustered:  return "clustered";
    case ClusterFollowerPhase::Grace:      return "grace";
    case ClusterFollowerPhase::LeaderLost: return leaderLostName;
    default:                               return "standalone";
  }
}
