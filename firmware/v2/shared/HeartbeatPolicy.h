#pragma once
// Pure I2C heartbeat / miss-detection logic (#310). I2C slaves can't initiate,
// so the master drives every read: displayTask reads one unit per idle tick
// (round-robin), and a per-unit consecutive-miss counter flags a unit that
// browned out / crashed / fell off the bus as stale/lost — which the old
// on-demand-only poll never noticed. Transport + Wire glue stay in
// Tasks.cpp / UnitBus.cpp; this header is the natively-tested schedule + miss
// contract (mirrors the UnitHealth.h / WearPolicy.h pure-seam split).
//
// SHARED header: copied verbatim into firmware/v2/FollowerEsp01 (copy policy —
// fix bugs in both trees). See spec
// docs/superpowers/specs/2026-07-15-unit-boot-home-stagger-and-i2c-heartbeat-health-design.md
// §2.

#include <stdint.h>

#include "UnitHealth.h"  // UnitFacts (the freshness fields heartbeatApply folds)

// How often displayTask synthesizes one heartbeat read when otherwise idle.
// Full fleet cadence = TICK_MS * width (~48 s for 16 units at 3 s). Tunable.
#define HEARTBEAT_TICK_MS         3000UL
// Consecutive-miss threshold K: this many failed reads in a row -> stale/lost
// (surfaced in the API + the HA fault sensor). Tunable 3..5.
#define HEARTBEAT_MISS_THRESHOLD  3

// Consecutive-miss counter update: a good read resets to 0, a miss (NACK /
// checksum fail / timeout) increments, saturating at 255.
inline uint8_t heartbeatMissCount(uint8_t prev, bool ok) {
  if (ok) return 0;
  return prev >= 0xFF ? 0xFF : (uint8_t)(prev + 1);
}

// A unit is stale/lost once it has missed >= threshold consecutive reads.
inline bool heartbeatIsStale(uint8_t misses, uint8_t threshold) {
  return misses >= threshold;
}

// Round-robin advance over display columns [0, width). Wraps at width; a
// zero/negative width parks at slot 0 (nothing to poll).
inline int heartbeatNextSlot(int slot, int width) {
  if (width <= 0) return 0;
  int next = slot + 1;
  return (next >= width) ? 0 : next;
}

// Folds one scheduled-read outcome into a unit's freshness bookkeeping. `ok`
// is whether the CMD_GET_STATUS read succeeded. ONLY sketch slots (state 1) are
// tracked; a gap resets so an empty column never latches stale. A good read
// resets the miss counter and re-stamps lastSeenMs (the /units/health "age"
// basis); a miss increments the counter and, past `threshold`, latches stale.
//
// The freshness fields are surfaced in buildUnitHealthJson under a state==1
// gate (NOT statusValid): a lost unit's whole point is that its current read
// FAILED, so gating the age/misses/stale keys on statusValid would make them
// unreachable exactly when they matter (the read that produced misses>0 had
// statusValid=false). Keep this pure so test_unit_health can drive a real miss
// streak end-to-end into the JSON.
inline void heartbeatApply(UnitFacts& u, bool ok, uint32_t nowMs,
                           uint8_t threshold) {
  if (u.state != 1) {
    u.misses = 0;
    u.stale = false;
    return;
  }
  u.misses = heartbeatMissCount(u.misses, ok);
  u.stale = heartbeatIsStale(u.misses, threshold);
  if (ok) u.lastSeenMs = nowMs;
}
