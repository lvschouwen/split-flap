#pragma once
// ReleasePolicy.h — when the master looks for a release and what it then
// knows (#583; docs/superpowers/specs/2026-10-10-release-update-design.md,
// section 6). Pure, natively tested by test_release_policy; ReleaseUpdate.cpp
// runs it on the worker task.
//
// It only looks by itself: installing always starts with an operator's
// update-from-release.

#include <stdint.h>
#include <string.h>

#include "ReleaseManifest.h"

// After a start, once the clock is set.
#define RELEASE_LOOK_FIRST_MS 120000UL
#define RELEASE_LOOK_EVERY_MS (24UL * 3600UL * 1000UL)
// After a look that failed.
#define RELEASE_LOOK_RETRY_MS (3600UL * 1000UL)

#define RELEASE_CHANNEL_DEFAULT "stable"

inline bool releaseChannelValid(const char* channel) {
  return releaseChannelDir(channel) != nullptr;
}

struct ReleaseSchedule {
  bool looked = false;
  bool failed = false;
  uint32_t lookedMs = 0;

  // Is a look by itself due? `enabled` is the releaseCheck setting.
  bool due(uint32_t nowMs, bool clockSet, bool enabled) const {
    if (!enabled || !clockSet) return false;
    if (!looked) return nowMs >= RELEASE_LOOK_FIRST_MS;
    return nowMs - lookedMs >= (failed ? RELEASE_LOOK_RETRY_MS : RELEASE_LOOK_EVERY_MS);
  }

  // Any look counts, asked for or not.
  void done(uint32_t nowMs, bool ok) {
    looked = true;
    failed = !ok;
    lookedMs = nowMs;
  }

  // Another channel was chosen: what was found says nothing about it.
  void reset() { *this = ReleaseSchedule(); }
};

enum class ReleaseLookState : uint8_t { NotLooked, UpToDate, Newer, Failed };

inline const char* releaseLookStateName(ReleaseLookState s) {
  switch (s) {
    case ReleaseLookState::UpToDate: return "up-to-date";
    case ReleaseLookState::Newer: return "newer";
    case ReleaseLookState::Failed: return "failed";
    default: return "not-looked";
  }
}

// What an update is doing. The order is the order it works in.
enum class ReleaseStep : uint8_t { None, Looking, Rescue, RowImage, Master, Restarting };

inline const char* releaseStepName(ReleaseStep s) {
  switch (s) {
    case ReleaseStep::Looking: return "looking";
    case ReleaseStep::Rescue: return "rescue";
    case ReleaseStep::RowImage: return "row-image";
    case ReleaseStep::Master: return "master";
    case ReleaseStep::Restarting: return "restarting";
    default: return "";
  }
}

struct ReleaseStatus {
  ReleaseLookState look = ReleaseLookState::NotLooked;
  ReleaseError why = ReleaseError::Ok;  // of a look that failed
  uint32_t lookedAtS = 0;               // wall clock, 0 = never
  ReleaseManifest release;              // whole while UpToDate or Newer
  // The update that runs, op 0 = none.
  uint32_t op = 0;
  ReleaseStep step = ReleaseStep::None;
  uint32_t done = 0;
  uint32_t size = 0;
};

// What a look found, folded into the status.
inline void releaseStatusFold(ReleaseStatus& status, ReleaseError result,
                              const ReleaseManifest& found, uint32_t runningCommitTime,
                              uint32_t nowS) {
  status.lookedAtS = nowS;
  status.why = result;
  if (result != ReleaseError::Ok) {
    status.look = ReleaseLookState::Failed;
    status.release = ReleaseManifest();
    return;
  }
  status.release = found;
  status.look = releaseNewer(found, runningCommitTime) ? ReleaseLookState::Newer
                                                       : ReleaseLookState::UpToDate;
}
