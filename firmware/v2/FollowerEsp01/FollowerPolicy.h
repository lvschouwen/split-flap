#pragma once
// FollowerPolicy.h — what the ESP-01 dumb row adds to the shared phase
// machine (ClusterMemberPhase.h; spec docs/superpowers/specs/2026-07-14-v2-
// esp01-follower-design.md), natively tested by test_follower_policy. One
// deliberate choice: a leader written off (LeaderLost, ~2 min of silence)
// BLANKS the row — it has no local content of its own, and a stale frozen row
// looks broken while the leader's health strip already shows it as lost.
// Never a takeover candidate: no promote gate exists here by design.

#include <stdint.h>

#include "ClusterMemberPhase.h"

// Standalone and LeaderLost show nothing; Grace holds the last text.
inline bool followerPhaseShowsBlank(ClusterFollowerPhase p) {
  return p == ClusterFollowerPhase::Standalone ||
         p == ClusterFollowerPhase::LeaderLost;
}

// The phase as the log words it: "blank" is this firmware's word for a
// master written off.
inline const char* followerPhaseName(ClusterFollowerPhase p) {
  return clusterFollowerPhaseName(p, "blank");
}
