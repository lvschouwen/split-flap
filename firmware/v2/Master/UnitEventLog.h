#pragma once
// Pure edge-detection for the master-side unit-health event log (#322). The
// master surfaces per-unit health only as passive /units/health JSON counters;
// a unit going faulty (last home failed / hall never fired), falling off the
// bus (#310 heartbeat), or disagreeing with its intended letter (#264 mm) never
// reaches the flash/web log an operator actually watches — the same silent gap
// as the drift auto re-home (DriftLogPolicy.h, the counter half of #322).
//
// This folds the current per-unit condition mask into the TRANSITIONS worth one
// log line since we last looked, so each onset (and each recovery, for the
// recoverable conditions) is logged exactly once. The last-logged mask lives in
// UnitFacts (per probe epoch; a probe rescan re-zeroes it, and a transient
// unreadable poll carries the prior via validMask — a unit going silent must
// never read as "recovered"). Natively tested by test_unit_event_log; the
// SerialPrintf glue + label strings stay in DisplayTask.cpp.

#include <stdint.h>

// Loggable per-unit conditions, one bit each (master-side derived).
#define UNIT_EVT_HOME_FAILED (1 << 0)  // UNIT_FLAG_LAST_HOME_FAILED (status)
#define UNIT_EVT_HALL_NEVER  (1 << 1)  // UNIT_FLAG_HALL_NEVER (status)
#define UNIT_EVT_STALE       (1 << 2)  // heartbeat lost — unit off the bus (#310)
#define UNIT_EVT_MISMATCH    (1 << 3)  // displayed letter != intended (#264 mm)

// Conditions whose RECOVERY (true->false) is also worth a log line. Onset is
// always logged; recovery only here — a unit rejoining the bus or homing OK
// again is real good-news, whereas hall-never is effectively permanent and a
// mismatch clearing is usually just the next frame catching up (noise).
#define UNIT_EVT_RECOVERABLE (UNIT_EVT_HOME_FAILED | UNIT_EVT_STALE)

struct UnitEventTransitions {
  uint8_t onset;     // conditions that became true since the baseline
  uint8_t recovery;  // recoverable conditions that became false
  uint8_t newState;  // condition mask to store back into the unit's facts
};

// prior == the last-logged condition mask for this unit (0 at a fresh probe
// epoch). cur == the conditions true right now. validMask == the conditions
// actually observable this poll: bits outside it keep their prior value, so a
// unit that went unreadable neither onsets nor "recovers" its status-derived
// conditions.
inline UnitEventTransitions unitEventEvaluate(uint8_t prior, uint8_t cur,
                                              uint8_t validMask) {
  uint8_t effective = (uint8_t)((cur & validMask) | (prior & ~validMask));
  UnitEventTransitions t;
  t.onset = (uint8_t)(effective & ~prior);
  t.recovery = (uint8_t)((prior & ~effective) & UNIT_EVT_RECOVERABLE);
  t.newState = effective;
  return t;
}
