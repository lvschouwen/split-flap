#pragma once
// Pure decision for the master-side drift-event operator log (#322). A unit
// silently auto re-homes on drift (#263): its own Serial line (Unit.ino) is
// invisible — units have no operator log surface — and the master folds the
// de/ds/dp diag counters into /units/health without ever emitting a log line,
// so a real drift/self-correction never reaches the flash/web log an operator
// watches. This turns the unit's monotonic, saturating per-unit drift-event
// counter (de) into "how many NEW events since we last looked", robustly:
//   - the baseline lives in UnitFacts, so it survives transient diag-read gaps
//     (a failed poll early-returns without touching it — the #322 "after a
//     leader recovery" scenario) and re-baselines only on a real probe rescan.
// Natively tested by test_drift_log; the SerialPrintf glue stays in UnitBus.cpp.

#include <stdint.h>

struct DriftLogDecision {
  bool shouldLog;       // emit one operator log line
  uint8_t newEvents;    // new drift events since the baseline (valid when logging)
  int16_t newBaseline;  // baseline to store back into the unit's facts
};

// baseline < 0  => no baseline yet (first valid diag read this probe epoch):
//                  adopt cur silently — a unit's since-boot history isn't "new".
// cur  > baseline => that many NEW events: log, advance the baseline.
// cur  < baseline => the unit rebooted (de reset): re-baseline, don't log —
//                    the reset itself is not a drift event.
// cur == baseline => nothing new.
inline DriftLogDecision driftLogEvaluate(int16_t baseline, uint8_t curEvents) {
  DriftLogDecision r{false, 0, (int16_t)curEvents};
  if (baseline >= 0 && (int)curEvents > (int)baseline) {
    r.shouldLog = true;
    r.newEvents = (uint8_t)((int)curEvents - (int)baseline);
  }
  return r;
}
