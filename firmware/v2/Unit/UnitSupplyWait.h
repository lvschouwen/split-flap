#pragma once
// Pure rule for holding a move back while the unit's own supply reads low
// (#505). Natively tested by test_supply_wait; the rotateToLetter() glue is
// bench tier. Runs only with UNIT_GATE_SUPPLY_WAIT set.
//
// Every unit of a row and its board share one 5 V rail. A unit that is about
// to switch its coils on reads the rail first; when it reads well under the
// unit's own idle level the unit reports busy and asks again on the next loop
// pass. The hold is bounded: a rail that stays low must not park the wall, so
// after the cap the move goes anyway.
//
// The line is relative, not a voltage. The reading comes from the chip's own
// reference, which differs from chip to chip: 21 units on two rails read
// 4.87 to 5.16 V at rest (2026-10-09), up to 0.29 V apart on one rail. Against
// its own idle level every unit holds at the same real dip.
//
// SUPPLY_WAIT_DROP_MV is twice the largest dip any of those units had
// recorded since its start (0.15 V, through homes and full frames). The cap
// is a choice, not a measurement.

#include <stdint.h>

#define SUPPLY_WAIT_DROP_MV 300
#define SUPPLY_WAIT_MAX_MS 2000UL

// The unit's idle level: fed the once-a-second reading taken with the coils
// off. It takes a higher reading at once and follows a lower one slowly (a
// 256th of the gap per reading, 1 mV at least), so a neighbour's move does
// not pull it down, and a rail that has settled lower for good stops holding
// every move after a minute or two. 0 = no level yet.
inline void supplyIdleLevelFold(uint16_t& levelMv, uint16_t idleMv) {
  if (idleMv == 0) return;
  if (levelMv == 0 || idleMv >= levelMv) {
    levelMv = idleMv;
    return;
  }
  uint16_t step = (uint16_t)((levelMv - idleMv) / 256);
  levelMv = (uint16_t)(levelMv - (step == 0 ? 1 : step));
}

struct SupplyWait {
  bool waiting = false;   // a hold is running for the move in hand
  bool held = false;      // that move was held at least once
  uint32_t sinceMs = 0;   // when its hold began
};

inline bool supplyReadsLow(uint16_t vccMv, uint16_t idleLevelMv) {
  return vccMv != 0 && idleLevelMv != 0 &&
         (uint32_t)vccMv + SUPPLY_WAIT_DROP_MV < idleLevelMv;
}

// Asked once per attempt to start a move, with a fresh reading and the idle
// level (0 = none of either: never hold). True = hold this pass and ask
// again.
inline bool supplyWaitHold(SupplyWait& s, uint16_t vccMv, uint16_t idleLevelMv,
                           uint32_t nowMs) {
  if (!supplyReadsLow(vccMv, idleLevelMv)) {
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
