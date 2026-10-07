#pragma once
// Pure timing rule for the last-move stall bit (#374). Natively tested by
// test_stall_policy; the rotateToLetter() glue that times the move is bench
// tier.
//
// The stepper library blocks one fixed period per step whatever the load, so
// a healthy move takes its step count times that period. A move that takes
// more than a quarter longer is reported as stalled. The expected time
// therefore has to hold every step the move makes: a move by way of home
// steps the calibration offset on from the marker before the flap steps, and
// for a move to blank those offset steps are all that follows the seek.

#include <stdint.h>

// Wall time of `steps` steps at `speedRpm` on a motor of `stepsPerRev`:
// step period = 60e6 / (stepsPerRev * rpm) us. Products stay below ~78M for a
// three-revolution seek, so uint32 is enough.
inline uint32_t stallExpectedMoveMs(uint32_t steps, int speedRpm, uint16_t stepsPerRev) {
  if (speedRpm < 1) speedRpm = 1;
  uint32_t stepUs = 60000000UL / ((uint32_t)stepsPerRev * (uint32_t)speedRpm);
  return steps * stepUs / 1000UL;
}

// A move by way of home: the seek to the marker and the calibration offset on
// from it at `homingRpm`, then the flap steps at `speedRpm`.
inline uint32_t stallExpectedSeekMoveMs(uint32_t homingSteps, int16_t offsetSteps,
                                        uint32_t flapSteps, int homingRpm, int speedRpm,
                                        uint16_t stepsPerRev) {
  uint32_t offset = offsetSteps < 0 ? (uint32_t)(-(int32_t)offsetSteps) : (uint32_t)offsetSteps;
  return stallExpectedMoveMs(homingSteps + offset, homingRpm, stepsPerRev) +
         stallExpectedMoveMs(flapSteps, speedRpm, stepsPerRev);
}

inline bool stallExceeded(uint32_t actualMs, uint32_t expectedMs) {
  return actualMs > expectedMs + (expectedMs >> 2);
}
