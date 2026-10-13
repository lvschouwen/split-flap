#pragma once
// WallLinkLogDamp.h — keeps one thing that repeats from filling a log. A
// link that drops and returns every minute writes its lines on both boards
// and pushes everything else out of their rings (#609). The first
// WL_LOG_DAMP_FULL of a period are written in full; the rest are counted
// and reported as one line when the period ends. Pure; tested by Master's
// test_wall_link_log_damp.

#include <stdint.h>

#define WL_LOG_DAMP_FULL 3
#define WL_LOG_DAMP_PERIOD_MS 1800000UL

struct WlLogDamp {
  bool running = false;    // a period is open
  uint32_t startedMs = 0;
  uint16_t seen = 0;       // in this period
  uint16_t held = 0;       // of those, counted and not written
};

// Closes a period that is over and gives how many it held back (0: nothing
// to report). Asked before every wlLogDampWrite and once per pass, so the
// report is written even when the thing has stopped.
inline uint16_t wlLogDampEnded(WlLogDamp& d, uint32_t nowMs) {
  if (!d.running || (uint32_t)(nowMs - d.startedMs) < WL_LOG_DAMP_PERIOD_MS) return 0;
  const uint16_t held = d.held;
  d = WlLogDamp{};
  return held;
}

// It happened once more: true when this one is written in full.
inline bool wlLogDampWrite(WlLogDamp& d, uint32_t nowMs) {
  if (!d.running) {
    d = WlLogDamp{};
    d.running = true;
    d.startedMs = nowMs;
  }
  if (d.seen < UINT16_MAX) d.seen++;
  if (d.seen <= WL_LOG_DAMP_FULL) return true;
  if (d.held < UINT16_MAX) d.held++;
  return false;
}

// The last one was held back: a line that only goes with it (the connection
// that followed) is held back too.
inline bool wlLogDampHolding(const WlLogDamp& d) {
  return d.running && d.seen > WL_LOG_DAMP_FULL;
}
