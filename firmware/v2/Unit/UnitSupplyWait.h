#pragma once
// Pure rule for holding a move back while the unit's own supply reads low
// (#505). Natively tested by test_supply_wait; the rotateToLetter() glue is
// bench tier. Runs only with UNIT_GATE_SUPPLY_WAIT set.
//
// Every unit of a row and its board share one 5 V rail. A unit that is about
// to switch its coils on reads the rail first; under the line it reports busy
// and asks again on the next loop pass. The hold is bounded: a rail that
// stays low must not park the wall, so after the cap the move goes anyway.
//
// The line is the one the row boards already judge a loaded rail by
// (MOTION_SAG_LOW_MV, shared/MotionBudget.h). The cap is a choice, not a
// measurement: no unit on the wall has read under the line yet.

#include <stdint.h>

#define SUPPLY_WAIT_LOW_MV 4700
#define SUPPLY_WAIT_MAX_MS 2000UL

struct SupplyWait {
  bool waiting = false;   // a hold is running for the move in hand
  bool held = false;      // that move was held at least once
  uint32_t sinceMs = 0;   // when its hold began
};

// Asked once per attempt to start a move, with a fresh idle reading
// (0 = none). True = hold this pass and ask again.
inline bool supplyWaitHold(SupplyWait& s, uint16_t vccMv, uint32_t nowMs) {
  if (vccMv == 0 || vccMv >= SUPPLY_WAIT_LOW_MV) {
    s.waiting = false;
    return false;
  }
  if (!s.waiting) {
    s.waiting = true;
    s.held = true;
    s.sinceMs = nowMs;
    return true;
  }
  if (nowMs - s.sinceMs >= SUPPLY_WAIT_MAX_MS) {
    s.waiting = false;
    return false;
  }
  return true;
}

// Whether the move now starting was held; clears it for the next move.
inline bool supplyWaitTakeHeld(SupplyWait& s) {
  bool held = s.held;
  s.held = false;
  return held;
}

// The move in hand was given up (its letter withdrawn).
inline void supplyWaitReset(SupplyWait& s) {
  s.waiting = false;
  s.held = false;
}
