#pragma once
// ClusterRolloutPolicy.h — pure fleet-firmware convergence logic (#276,
// epic #270; spec docs/superpowers/specs/2026-07-13-multi-display-cluster-
// design.md), natively tested by test_cluster_rollout_policy. The leader's
// RUNNING build is the cluster version, enforced in BOTH directions — a
// deliberately newer follower is converged back (uncluster first to
// bench-test a follower build). No flash or HTTP here: the streaming
// upload lives in ClusterLeader.cpp's clusterTask; this header owns the
// candidate selection, the strictly-sequential phase machine, the rejoin
// health gate, and the multipart wire constants.
//
// Sequencing contract (spec): one member at a time; the next candidate is
// picked only after the previous one rejoined reporting the new rev. A bad
// image strands at most one board, and native A/B rollback un-strands it —
// a rollback shows up here as a rejoin on the OLD rev, which burns an
// attempt so a poisoned image can't flash-loop the same board forever.

#include <Arduino.h>

#include "ClusterLeaderPolicy.h"  // ClusterMemberRuntime, table types

// Attempts per member before the rollout gives up on it (surfaced as
// `updateBlocked` in /cluster/status; cleared by a config swap or leader
// reboot). 3 matches the degraded-after-3 supervision convention.
static const uint8_t CLUSTER_ROLLOUT_ATTEMPT_CAP = 3;

// Rejoin budget after a successful upload: reboot (~10 s) + PENDING_VERIFY
// confirm on netif-up + the leader's join backoff ladder fit well inside.
static const uint32_t CLUSTER_ROLLOUT_REJOIN_TIMEOUT_MS = 120000UL;

// Global pause after a failed/rejected upload before the next candidate is
// considered — a busy follower (409: its own reflash/OTA in flight) or a
// flaky link shouldn't be hammered at tick cadence.
static const uint32_t CLUSTER_ROLLOUT_RETRY_HOLDOFF_MS = 30000UL;

// Streamed bytes per clusterTask tick (100 ms): ~480 KB/s ceiling, so a
// ~2.6 MB image takes ~6 s while renders/pings keep flowing between ticks.
static const uint32_t CLUSTER_ROLLOUT_CHUNK_PER_TICK = 49152UL;

// Upload round-trips wait on the follower's flash writes and final MD5
// verify — the 1.5 s render timeout is far too tight here.
static const int CLUSTER_ROLLOUT_HTTP_TIMEOUT_MS = 10000;

#define CLUSTER_ROLLOUT_BOUNDARY "splitflapClusterRollout"

enum class ClusterRolloutPhase : uint8_t { Idle = 0, Uploading, WaitingRejoin };

enum class ClusterRolloutWait : uint8_t {
  Waiting = 0,   // member not back yet, deadline not hit
  Converged,     // rejoined on the leader's rev — success, next member
  RolledBack,    // rejoined on the OLD rev — image rejected, attempt burned
  TimedOut       // never rejoined inside the budget — attempt burned
};

struct ClusterRolloutState {
  ClusterRolloutPhase phase = ClusterRolloutPhase::Idle;
  int memberIndex = -1;  // valid while phase != Idle
  uint8_t attempts[CLUSTER_MAX_MEMBERS] = {0};
  bool blocked[CLUSTER_MAX_MEMBERS] = {false};
  uint32_t holdoffUntilMs = 0;   // candidate gate after a failure/rejection
  uint32_t waitDeadlineMs = 0;   // WaitingRejoin budget
  uint32_t bytesSent = 0;        // Uploading progress (status/UI)
  uint32_t bytesTotal = 0;
};

// First convergence candidate, or -1. Strictly sequential: only while
// Idle and past the holdoff. A candidate must be reachable (joined), have
// reported a rev (a pre-rev follower is never guessed at), differ from the
// leader's rev in EITHER direction, and not be given up on.
inline int clusterRolloutNextCandidate(const ClusterMemberTable& table,
                                       const ClusterMemberRuntime* runtimes,
                                       const char* leaderRev,
                                       const ClusterRolloutState& st,
                                       uint32_t nowMs) {
  if (st.phase != ClusterRolloutPhase::Idle) return -1;
  if ((int32_t)(nowMs - st.holdoffUntilMs) < 0) return -1;
  for (int i = 0; i < table.count; i++) {
    if (clusterMemberIsSelf(table.members[i])) continue;
    if (!runtimes[i].joined) continue;
    if (runtimes[i].rev.length() == 0) continue;
    if (runtimes[i].rev == leaderRev) continue;
    if (st.blocked[i]) continue;
    return i;
  }
  return -1;
}

inline void clusterRolloutStart(ClusterRolloutState& st, int memberIndex,
                                uint32_t totalBytes) {
  st.phase = ClusterRolloutPhase::Uploading;
  st.memberIndex = memberIndex;
  st.bytesSent = 0;
  st.bytesTotal = totalBytes;
}

inline void clusterRolloutBurnAttempt(ClusterRolloutState& st,
                                      uint32_t nowMs) {
  int i = st.memberIndex;
  if (i >= 0 && i < CLUSTER_MAX_MEMBERS) {
    if (st.attempts[i] < 255) st.attempts[i]++;
    if (st.attempts[i] >= CLUSTER_ROLLOUT_ATTEMPT_CAP) st.blocked[i] = true;
  }
  st.phase = ClusterRolloutPhase::Idle;
  st.memberIndex = -1;
  st.holdoffUntilMs = nowMs + CLUSTER_ROLLOUT_RETRY_HOLDOFF_MS;
}

// Transport failure or non-2xx/409 verdict: counts toward the cap.
inline void clusterRolloutUploadFailed(ClusterRolloutState& st,
                                       uint32_t nowMs) {
  clusterRolloutBurnAttempt(st, nowMs);
}

// 409 from the follower (its own reflash/OTA owns the flash right now):
// transient by contract — holdoff only, no attempt burned.
inline void clusterRolloutUploadRejected(ClusterRolloutState& st,
                                         uint32_t nowMs) {
  st.phase = ClusterRolloutPhase::Idle;
  st.memberIndex = -1;
  st.holdoffUntilMs = nowMs + CLUSTER_ROLLOUT_RETRY_HOLDOFF_MS;
}

// 200 from the follower: image verified and armed, reboot incoming — the
// health gate takes over.
inline void clusterRolloutUploadDone(ClusterRolloutState& st, uint32_t nowMs) {
  st.phase = ClusterRolloutPhase::WaitingRejoin;
  st.waitDeadlineMs = nowMs + CLUSTER_ROLLOUT_REJOIN_TIMEOUT_MS;
}

// The health gate: fed the target member's live supervision facts each
// tick while WaitingRejoin. Success clears the member's attempts.
inline ClusterRolloutWait clusterRolloutCheckWait(ClusterRolloutState& st,
                                                  bool memberJoined,
                                                  const String& memberRev,
                                                  const char* leaderRev,
                                                  uint32_t nowMs) {
  int i = st.memberIndex;
  if (memberJoined && memberRev == leaderRev) {
    if (i >= 0 && i < CLUSTER_MAX_MEMBERS) {
      st.attempts[i] = 0;
      st.blocked[i] = false;
    }
    st.phase = ClusterRolloutPhase::Idle;
    st.memberIndex = -1;
    return ClusterRolloutWait::Converged;
  }
  if (memberJoined && memberRev.length() > 0) {
    clusterRolloutBurnAttempt(st, nowMs);
    return ClusterRolloutWait::RolledBack;
  }
  if ((int32_t)(nowMs - st.waitDeadlineMs) >= 0) {
    clusterRolloutBurnAttempt(st, nowMs);
    return ClusterRolloutWait::TimedOut;
  }
  return ClusterRolloutWait::Waiting;
}

// Config swaps and aborts: forget everything, including give-ups — the
// new table deserves a fresh look at every member.
inline void clusterRolloutReset(ClusterRolloutState& st) {
  st = ClusterRolloutState{};
}

inline const char* clusterRolloutPhaseName(ClusterRolloutPhase phase) {
  switch (phase) {
    case ClusterRolloutPhase::Uploading: return "uploading";
    case ClusterRolloutPhase::WaitingRejoin: return "waiting";
    default: return "idle";
  }
}

// --- multipart wire bits -----------------------------------------------------------
// The follower's EXISTING POST /firmware/master parses multipart field
// "firmware" with a mandatory ?md5= — zero new follower code (spec).

inline String clusterRolloutMultipartPreamble() {
  return String("--" CLUSTER_ROLLOUT_BOUNDARY "\r\n"
                "Content-Disposition: form-data; name=\"firmware\"; "
                "filename=\"firmware.bin\"\r\n"
                "Content-Type: application/octet-stream\r\n\r\n");
}

inline String clusterRolloutMultipartTrailer() {
  return String("\r\n--" CLUSTER_ROLLOUT_BOUNDARY "--\r\n");
}

// ?v= is the endpoint's existing intendedVersion field — the follower
// records what this flash meant to install (and the fake-follower bench
// harness adopts it as its post-reboot rev, #278).
inline String clusterRolloutUrl(const String& host, const String& md5,
                                const char* leaderRev) {
  return "http://" + host + "/firmware/master?md5=" + md5 + "&v=" + leaderRev;
}
