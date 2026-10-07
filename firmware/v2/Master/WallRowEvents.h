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
};

// "?" for a code this build does not know (a newer row).
inline const char* wallRowEventName(uint32_t code) {
  for (const WallRowEventName& e : WALL_ROW_EVENT_NAMES) {
    if ((uint32_t)e.code == code) return e.name;
  }
  return "?";
}

// What is kept of a row's code in an entry's one byte: a code past it is
// kept as 0, which names nothing.
inline uint8_t wallRowEventDetail(uint32_t code) { return code <= 255 ? (uint8_t)code : 0; }
