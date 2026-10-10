#pragma once
// WallRowEvents.h — the names of what a row board reports about itself
// (RowEventCode in wall_link.proto), for GET /api/v2/history (#570). Pure,
// natively tested by test_wall_row_events; tests/test_wall_jobs_glue.py
// fails when the schema gains a code this table does not name.

#include <stdint.h>

#include "wall_link.pb.h"

struct WallRowEventName {
  wl_RowEventCode code;
  const char* name;
};

static const WallRowEventName WALL_ROW_EVENT_NAMES[] = {
    {wl_RowEventCode_ROW_EVT_STARTED, "started"},
    {wl_RowEventCode_ROW_EVT_SELF_RESTART, "self-restart"},
    {wl_RowEventCode_ROW_EVT_LOW_MEMORY, "low-memory"},
    {wl_RowEventCode_ROW_EVT_BUS_DEAD, "bus-dead"},
    {wl_RowEventCode_ROW_EVT_BUS_LINES, "bus-lines"},
};

// "?" for a code this build does not know (a newer row).
inline const char* wallRowEventName(uint32_t code) {
  for (const WallRowEventName& e : WALL_ROW_EVENT_NAMES) {
    if ((uint32_t)e.code == code) return e.name;
  }
  return "?";
}

// How many entries one row may add to the record. A row has four places to
// queue events in (FollowerEvents.h), so twice that at once covers a restart
// straight after another; beyond it one every half minute, which a row that
// restarts in a loop does not reach either. What comes faster is a row gone
// wrong, and the record is for the whole wall: it is not written.
#define WALL_ROW_EVENT_BURST 8
#define WALL_ROW_EVENT_REFILL_MS 30000UL

struct WallRowEventBudget {
  uint8_t left = WALL_ROW_EVENT_BURST;
  uint32_t refilledMs = 0;

  // May this event be recorded? Takes one from the budget when so.
  bool take(uint32_t nowMs) {
    // A full budget earns nothing: the wait for the next one starts at its
    // first use.
    if (left == WALL_ROW_EVENT_BURST) refilledMs = nowMs;
    const uint32_t earned = (uint32_t)(nowMs - refilledMs) / WALL_ROW_EVENT_REFILL_MS;
    if (earned > 0) {
      left = (uint8_t)(earned >= (uint32_t)(WALL_ROW_EVENT_BURST - left) ? WALL_ROW_EVENT_BURST
                                                                         : left + earned);
      refilledMs += earned * WALL_ROW_EVENT_REFILL_MS;
    }
    if (left == 0) return false;
    left--;
    return true;
  }
};

// What is kept of a row's code in an entry's one byte: a code past it is
// kept as 0, which names nothing.
inline uint8_t wallRowEventDetail(uint32_t code) { return code <= 255 ? (uint8_t)code : 0; }
