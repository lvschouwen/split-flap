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
// `updateBlocked` in /cluster/status; cleared by a config swap, a leader
// reboot, or the member's reported rev changing — #340). 3 matches the
// degraded-after-3 supervision convention.
static const uint8_t CLUSTER_ROLLOUT_ATTEMPT_CAP = 3;

// Rejoin budget after a successful upload: reboot (~10 s) + PENDING_VERIFY
// confirm (pre-inrush in setup(), #305) + the leader's join backoff ladder
// fit well inside.
static const uint32_t CLUSTER_ROLLOUT_REJOIN_TIMEOUT_MS = 120000UL;

// Global pause after a failed/rejected upload before the next candidate is
// considered — a busy follower (409: its own reflash/OTA in flight) or a
// flaky link shouldn't be hammered at tick cadence.
static const uint32_t CLUSTER_ROLLOUT_RETRY_HOLDOFF_MS = 30000UL;

// Streamed bytes per clusterTask tick (100 ms): ~480 KB/s ceiling, so a
// ~2.6 MB image takes ~6 s while renders/pings keep flowing between ticks.
static const uint32_t CLUSTER_ROLLOUT_CHUNK_PER_TICK = 49152UL;

// Finalize verdict wait ONLY (trailer + response headers): the follower's
// Update.end() runs a whole-image MD5 verify (~1-2 s) before it answers.
static const int CLUSTER_ROLLOUT_FINALIZE_TIMEOUT_MS = 10000;

// Chunk-write socket timeout for the firmware streams (#276 rollout + #304
// relay) ONLY — ping/render keep their 1.5 s LAN bound. The receiver's
// early flash-sector erases can stall the socket for seconds (#340: 1.5 s
// killed streams ~2 s in while a patient manual OTA succeeded); each write
// is wdtFeed-bracketed and the per-tick pump is wall-clock bounded, so the
// longer wait never threatens the 30 s TWDT.
static const int CLUSTER_ROLLOUT_STREAM_TIMEOUT_MS = 8000;

enum class ClusterRolloutPhase : uint8_t { Idle = 0, Uploading, WaitingRejoin };

enum class ClusterRolloutWait : uint8_t {
  Waiting = 0,   // member not back yet, deadline not hit
  Converged,     // rejoined on the leader's rev — success, next member
  RolledBack,    // rejoined on the OLD rev — image rejected, attempt burned
  TimedOut,      // never rejoined inside the budget — attempt burned
  RescueLooping  // #343: rejoined still beaconing — image didn't take,
                 // attempt burned (cap stops a poisoned store image)
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

// A member on a DIFFERENT platform (#297): its reported plat is non-empty
// and differs from the leader's own. Absent plat = same platform — the
// pre-#297 S3 fleet keeps converging unchanged.
inline bool clusterMemberPlatForeign(const String& memberPlat,
                                     const char* leaderPlat) {
  return memberPlat.length() > 0 && memberPlat != leaderPlat;
}

// First convergence candidate, or -1. Strictly sequential: only while
// Idle and past the holdoff. A candidate must be reachable (joined), have
// reported a rev (a pre-rev follower is never guessed at), differ from the
// leader's rev in EITHER direction, be on the leader's own platform (#297
// — the payload IS the leader's running image), and not be given up on.
inline int clusterRolloutNextCandidate(const ClusterMemberTable& table,
                                       const ClusterMemberRuntime* runtimes,
                                       const char* leaderRev,
                                       const char* leaderPlat,
                                       const ClusterRolloutState& st,
                                       uint32_t nowMs) {
  if (st.phase != ClusterRolloutPhase::Idle) return -1;
  if ((int32_t)(nowMs - st.holdoffUntilMs) < 0) return -1;
  for (int i = 0; i < table.count; i++) {
    if (clusterMemberIsSelf(table.members[i])) continue;
    if (!runtimes[i].joined) continue;
    if (runtimes[i].rev.length() == 0) continue;
    if (runtimes[i].rev == leaderRev) continue;
    if (clusterMemberPlatForeign(runtimes[i].plat, leaderPlat)) continue;
    if (st.blocked[i]) continue;
    return i;
  }
  return -1;
}

// #344: esp01 auto-convergence — the SAME sequencing machine (shared state,
// so the fleet still converges strictly one member at a time and attempts/
// blocked behave identically), fed by the STORED follower image (#304)
// instead of the leader's running slot. Scanned only when the S3 scan found
// nothing. A candidate must report exactly the follower platform (inverse
// of the #297 guard — never hand the esp01 image to an S3) and a rev that
// differs from the stored image's rev, both directions like #276.
inline int clusterFollowerImageNextCandidate(
    const ClusterMemberTable& table, const ClusterMemberRuntime* runtimes,
    const char* storedRev, const char* followerPlat,
    const ClusterRolloutState& st, uint32_t nowMs) {
  if (st.phase != ClusterRolloutPhase::Idle) return -1;
  if ((int32_t)(nowMs - st.holdoffUntilMs) < 0) return -1;
  if (storedRev == nullptr || storedRev[0] == '\0') return -1;
  for (int i = 0; i < table.count; i++) {
    if (clusterMemberIsSelf(table.members[i])) continue;
    if (!runtimes[i].joined) continue;
    if (runtimes[i].rev.length() == 0) continue;
    if (runtimes[i].plat != followerPlat) continue;
    // #343: a rescue beacon needs the stored image back even at the same
    // rev — its flash is what's in doubt, not its version.
    if (runtimes[i].rev == storedRev && !runtimes[i].rescue) continue;
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

// #343 (review HIGH): a RESCUE-triggered push burns its attempt AT START.
// The rejoin verdict can't be trusted to count it — a poisoned image often
// joins looking healthy (rescue:0, matching rev) and only crashes after
// the handshake, which would read as Converged and reset the counter every
// cycle. Burning up front makes the cap accumulate across whole rescue
// cycles; clusterRolloutForgiveFollowerTargets is the deliberate reset.
inline void clusterRolloutStartRescue(ClusterRolloutState& st, int memberIndex,
                                      uint32_t totalBytes) {
  clusterRolloutStart(st, memberIndex, totalBytes);
  if (memberIndex >= 0 && memberIndex < CLUSTER_MAX_MEMBERS) {
    if (st.attempts[memberIndex] < 255) st.attempts[memberIndex]++;
    if (st.attempts[memberIndex] >= CLUSTER_ROLLOUT_ATTEMPT_CAP) {
      st.blocked[memberIndex] = true;  // this push is the last one allowed
    }
  }
}

// #343: a NEW stored follower image voids the poisoned-image evidence the
// rescue give-ups were built on — esp01 members get fresh attempts; S3
// members (converging from the leader's own slot) are untouched.
inline void clusterRolloutForgiveFollowerTargets(
    ClusterRolloutState& st, const ClusterMemberTable& table,
    const ClusterMemberRuntime* runtimes, const char* followerPlat) {
  for (int i = 0; i < table.count && i < CLUSTER_MAX_MEMBERS; i++) {
    if (runtimes[i].plat != followerPlat) continue;
    st.attempts[i] = 0;
    st.blocked[i] = false;
  }
}

// Progress counters are zeroed on EVERY transition to Idle — stale bytes
// next to phase "idle" would read as a wedged rollout in the Cluster card.
inline void clusterRolloutClearProgress(ClusterRolloutState& st) {
  st.bytesSent = 0;
  st.bytesTotal = 0;
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
  clusterRolloutClearProgress(st);
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
  clusterRolloutClearProgress(st);
}

// 200 from the follower: image verified and armed, reboot incoming — the
// health gate takes over.
inline void clusterRolloutUploadDone(ClusterRolloutState& st, uint32_t nowMs) {
  st.phase = ClusterRolloutPhase::WaitingRejoin;
  st.waitDeadlineMs = nowMs + CLUSTER_ROLLOUT_REJOIN_TIMEOUT_MS;
}

// The health gate: fed the target member's live supervision facts each
// tick while WaitingRejoin. Success clears the member's attempts — EXCEPT
// after a rescue-triggered push (#343 review HIGH): its "healthy" rejoin
// proves only that the handshake ran, not that the crash is gone, so the
// start-burned attempt stands until a genuine rev change or a new stored
// image forgives it. A rejoin still carrying the rescue marker means the
// pushed image didn't cure the boot loop — that outranks a matching rev.
inline ClusterRolloutWait clusterRolloutCheckWait(ClusterRolloutState& st,
                                                  bool memberJoined,
                                                  const String& memberRev,
                                                  bool memberRescue,
                                                  bool rescueTriggered,
                                                  const char* leaderRev,
                                                  uint32_t nowMs) {
  int i = st.memberIndex;
  if (memberJoined && memberRescue) {
    clusterRolloutBurnAttempt(st, nowMs);
    return ClusterRolloutWait::RescueLooping;
  }
  if (memberJoined && memberRev == leaderRev) {
    if (!rescueTriggered && i >= 0 && i < CLUSTER_MAX_MEMBERS) {
      st.attempts[i] = 0;
      st.blocked[i] = false;
    }
    st.phase = ClusterRolloutPhase::Idle;
    st.memberIndex = -1;
    clusterRolloutClearProgress(st);
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

// #340: a member's reported rev CHANGING re-proves the convergence facts —
// blocked/attempts were latched against a build the member no longer runs
// (it converged via manual OTA, or was bench-flashed to something new), so
// the candidate scan deserves a fresh look. Called wherever the leader
// records a reported rev. "" -> rev (the rejoin after OUR upload) is NOT a
// change: forgiving it would defeat the attempt cap that stops a poisoned
// image flash-looping one board.
inline void clusterRolloutNoteMemberRev(ClusterRolloutState& st, int i,
                                        const String& oldRev,
                                        const String& newRev) {
  if (i < 0 || i >= CLUSTER_MAX_MEMBERS) return;
  if (oldRev.length() == 0 || newRev.length() == 0) return;
  if (oldRev == newRev) return;
  st.attempts[i] = 0;
  st.blocked[i] = false;
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
//
// The boundary derives at RUNTIME from the image md5 (#292): the payload is
// this very firmware image, so any compile-time boundary constant exists
// inside it as a string literal and the follower's parser ends the file
// field right there (full-length body, truncated payload, MD5 fail). An
// image cannot contain its own md5.

inline String clusterRolloutBoundary(const String& imageMd5) {
  return "sfr-" + imageMd5;  // 36 chars, inside the RFC 2046 §5.1 cap of 70
}

inline String clusterRolloutContentType(const String& boundary) {
  return "multipart/form-data; boundary=" + boundary;
}

inline String clusterRolloutMultipartPreamble(const String& boundary) {
  return "--" + boundary +
         "\r\n"
         "Content-Disposition: form-data; name=\"firmware\"; "
         "filename=\"firmware.bin\"\r\n"
         "Content-Type: application/octet-stream\r\n\r\n";
}

inline String clusterRolloutMultipartTrailer(const String& boundary) {
  return "\r\n--" + boundary + "--\r\n";
}

// ?v= is the endpoint's existing intendedVersion field — the follower
// records what this flash meant to install (and the fake-follower bench
// harness adopts it as its post-reboot rev, #278).
inline String clusterRolloutUrl(const String& host, const String& md5,
                                const char* leaderRev) {
  return "http://" + host + "/firmware/master?md5=" + md5 + "&v=" + leaderRev;
}
