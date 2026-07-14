#pragma once
// FollowerPolicy.h — the ESP-01 dumb row's cluster phase machine (#298,
// epic #270; spec docs/superpowers/specs/2026-07-14-v2-esp01-follower-
// design.md), natively tested by test_follower_policy. Trimmed COPY of the
// v2 master's ClusterFollowerPolicy.h (copy policy: fix shared bugs in both
// trees) with one deliberate divergence: prolonged leader silence ends in
// **Blank** — this row has no local clock/content, and a stale frozen row
// looks broken while the leader's health strip already shows it as lost.
// Never a takeover candidate: no promote gate exists here by design.
//
// Phases: Standalone (never joined / left — blank) → Clustered (leader
// feeding) → Grace (leader silent, HOLD the last segment) → Blank (leader
// gone ~2 min — blank the row). Any leader contact reclaims straight to
// Clustered. A reboot with a persisted membership boots into Grace.
//
// Staleness armor (verbatim v2 rule): `epoch` is a random ID minted at
// leader boot, `seq` monotonic within it. Same-epoch renders with
// seq <= lastSeq are duplicates but still count as leader contact; ANY new
// epoch is accepted (leader rebooted).

#include <stdint.h>

// Leader pings ~10 s when idle; ~2.5 missed pings parks Clustered into
// Grace, and ~2 min of total silence blanks the row. Same constants as the
// v2 follower so the wall degrades uniformly.
static const uint32_t FOLLOWER_CONTACT_FRESH_MS = 25000UL;
static const uint32_t FOLLOWER_GRACE_MS = 120000UL;

// A commitAtMs further out than this is a bogus timestamp, not a
// synchronized flip — clamp instead of parking the display.
static const uint32_t FOLLOWER_COMMIT_MAX_DELAY_MS = 5000UL;

enum class FollowerPhase : uint8_t {
  Standalone = 0,
  Clustered,
  Grace,
  Blank,
};

enum class FollowerRenderVerdict : uint8_t {
  Apply = 0,     // fresh: render the segment
  Duplicate,     // stale epoch/seq pair: contact only, don't re-render
  NotClustered,  // no membership: leader must POST /cluster/join first
};

struct FollowerClusterState {
  FollowerPhase phase = FollowerPhase::Standalone;
  uint32_t epoch = 0;
  bool haveEpoch = false;
  uint32_t lastSeq = 0;
  uint32_t lastContactMs = 0;
};

// Boot entry: with a persisted membership the follower comes up gated in
// Grace (the leader will re-join/re-render); without one, Standalone.
inline void followerClusterBoot(FollowerClusterState& st, uint32_t nowMs,
                                bool membershipStored) {
  st = FollowerClusterState{};
  if (membershipStored) {
    st.phase = FollowerPhase::Grace;
    st.lastContactMs = nowMs;  // the grace window runs from boot
  }
}

// Accepted POST /cluster/join. Seq tracking resets ONLY on a new epoch: a
// same-epoch re-join (leader recovering a degraded member, no reboot) must
// keep rejecting delayed retries of old renders.
inline void followerClusterJoin(FollowerClusterState& st, uint32_t nowMs,
                                uint32_t epoch) {
  st.phase = FollowerPhase::Clustered;
  if (!st.haveEpoch || epoch != st.epoch) {
    st.epoch = epoch;
    st.haveEpoch = true;
    st.lastSeq = 0;
  }
  st.lastContactMs = nowMs;
}

// Any authenticated-by-membership leader contact (ping, render…). Returns
// false in Standalone — the reply tells the leader to re-join.
inline bool followerClusterContact(FollowerClusterState& st, uint32_t nowMs) {
  if (st.phase == FollowerPhase::Standalone) return false;
  st.phase = FollowerPhase::Clustered;  // reclaim is seamless
  st.lastContactMs = nowMs;
  return true;
}

// POST /cluster/render acceptance. Duplicates still feed the grace timer.
inline FollowerRenderVerdict followerClusterAcceptRender(
    FollowerClusterState& st, uint32_t nowMs, uint32_t epoch, uint32_t seq) {
  if (st.phase == FollowerPhase::Standalone) {
    return FollowerRenderVerdict::NotClustered;
  }
  followerClusterContact(st, nowMs);
  if (st.haveEpoch && epoch == st.epoch && seq <= st.lastSeq) {
    return FollowerRenderVerdict::Duplicate;
  }
  st.epoch = epoch;  // adopts any NEW epoch: the leader rebooted
  st.haveEpoch = true;
  st.lastSeq = seq;
  return FollowerRenderVerdict::Apply;
}

// POST /cluster/leave: back to Standalone (blank).
inline void followerClusterLeave(FollowerClusterState& st) {
  st = FollowerClusterState{};
}

// ~1 Hz supervision: Clustered decays to Grace, Grace to Blank. Returns
// true when the phase changed (glue logs it and blanks on Blank). Cascades
// on total silence, not tick cadence — a starved tick can't stretch grace.
inline bool followerClusterTick(FollowerClusterState& st, uint32_t nowMs) {
  uint32_t silence = nowMs - st.lastContactMs;
  bool changed = false;
  if (st.phase == FollowerPhase::Clustered &&
      silence >= FOLLOWER_CONTACT_FRESH_MS) {
    st.phase = FollowerPhase::Grace;
    changed = true;
  }
  if (st.phase == FollowerPhase::Grace && silence >= FOLLOWER_GRACE_MS) {
    st.phase = FollowerPhase::Blank;
    changed = true;
  }
  return changed;
}

// Sticky leadership (#295 semantics kept): a join from a DIFFERENT leader
// is rejected only while the current one is demonstrably alive. Grace and
// Blank mean the leader has gone silent — a promoted successor may claim
// this row; the same leader always may.
inline bool followerClusterJoinConflicts(const FollowerClusterState& st,
                                         uint32_t nowMs, bool sameLeader) {
  if (sameLeader) return false;
  if (st.phase != FollowerPhase::Clustered) return false;
  return nowMs - st.lastContactMs < FOLLOWER_CONTACT_FRESH_MS;
}

// Standalone and Blank show nothing; Grace holds the last text.
inline bool followerPhaseShowsBlank(FollowerPhase p) {
  return p == FollowerPhase::Standalone || p == FollowerPhase::Blank;
}

// Synchronized flip (verbatim v2 math): ms to wait before rendering.
// Unsynced clocks render immediately, past timestamps too; far-future ones
// clamp to FOLLOWER_COMMIT_MAX_DELAY_MS.
inline uint32_t followerRenderDelayMs(uint64_t commitAtMs, uint64_t nowEpochMs,
                                      bool timeSynced) {
  if (!timeSynced || commitAtMs <= nowEpochMs) return 0;
  uint64_t delay = commitAtMs - nowEpochMs;
  if (delay > FOLLOWER_COMMIT_MAX_DELAY_MS) return FOLLOWER_COMMIT_MAX_DELAY_MS;
  return (uint32_t)delay;
}

// /cluster/health + /settings state vocabulary. "blank" is this firmware's
// own word (the v2 follower says "local-fallback" there) — nothing parses
// it; humans and the member panel just display it.
inline const char* followerPhaseName(FollowerPhase p) {
  switch (p) {
    case FollowerPhase::Clustered: return "clustered";
    case FollowerPhase::Grace:     return "grace";
    case FollowerPhase::Blank:     return "blank";
    default:                       return "standalone";
  }
}
