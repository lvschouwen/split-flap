#pragma once
// WallShowPolicy.h — when the rows of a wall flip, and what the master does
// with its own row (#559/#566). Pure, natively tested by
// test_wall_show_policy; the glue is WallShow.cpp.
//
// Every row of a wall flips at one instant the master names (Unix ms, in
// Show.commit_at_ms and for its own row alike). Typed text gets a short lead.
// The clock's minute change is known in advance, so it is given to the rows
// seconds ahead, to flip at the boundary itself: one late message in a
// hundred (spec section 9) then no longer shows.

#include <stdint.h>
#include <string.h>
#include <time.h>

#include "ClusterMemberPhase.h"  // CLUSTER_COMMIT_MAX_DELAY_MS: what a row will wait

// The lead for text that is to show now (#273).
#define WALL_COMMIT_LEAD_MS 400UL
// From this long before a minute the clock is given the next minute. 2 s is
// the lead every row must get; the ticker runs once a second, so it first
// sees this window up to 1 s late.
#define WALL_CLOCK_AHEAD_MS 3000UL

// The flip instant for content that is to show now; 0 (on arrival) when this
// master's clock is not synced.
inline uint64_t wallCommitAtMs(uint64_t nowEpochMs, bool synced) {
  return synced ? nowEpochMs + WALL_COMMIT_LEAD_MS : 0;
}

struct WallClockTarget {
  time_t minuteEpochS = 0;  // the minute the wall is to show
  uint64_t commitAtMs = 0;  // when it flips to it
};

// What a clock wall should be given at this moment (a synced clock only).
// Calling it every second with the result deduplicated by content gives one
// Show per minute.
inline WallClockTarget wallClockTarget(uint64_t nowEpochMs) {
  const uint64_t minuteMs = nowEpochMs - nowEpochMs % 60000ULL;
  WallClockTarget target;
  if (nowEpochMs - minuteMs >= 60000ULL - WALL_CLOCK_AHEAD_MS) {
    target.minuteEpochS = (time_t)(minuteMs / 1000ULL + 60);
    target.commitAtMs = minuteMs + 60000ULL;
  } else {
    target.minuteEpochS = (time_t)(minuteMs / 1000ULL);
    target.commitAtMs = nowEpochMs + WALL_COMMIT_LEAD_MS;
  }
  return target;
}

// ---- the master's own row -------------------------------------------------------

struct WallOwnRow {
  const char* text = "";         // what the wall wants on this row
  const char* displayText = "";  // what the display says it shows
  bool pending = false;          // `text` is new and not queued yet
  uint32_t msUntilDue = 0;       // to its flip instant
  bool reflashing = false;       // a unit update owns the display
  bool notification = false;     // a show-then-revert overlay is up
  bool quiet = false;
  bool displayBusy = false;
};

enum class WallOwnAction : uint8_t { None, Wait, Show };

// Show = queue `text` for the display now. A pending text is queued at its
// instant; after that the row is given its text again whenever it shows
// something else (a notification ended, the units were homed), but never over
// a notification, while quiet, or into a busy display.
inline WallOwnAction wallOwnRowAction(const WallOwnRow& o) {
  if (o.pending) {
    if (o.msUntilDue > 0 || o.reflashing) return WallOwnAction::Wait;
    return WallOwnAction::Show;
  }
  if (o.text[0] == 0) return WallOwnAction::None;
  if (o.notification || o.quiet || o.displayBusy || o.reflashing) return WallOwnAction::None;
  return strcmp(o.text, o.displayText) == 0 ? WallOwnAction::None : WallOwnAction::Show;
}
