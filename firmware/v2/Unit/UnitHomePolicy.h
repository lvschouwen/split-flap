#pragma once
// Pure policy for a drum whose home failed (#502). Natively tested by
// test_home_policy; the calibrate()/loop() glue that acts on it is bench tier.
//
// A failed seek drives the motor through three full revolutions before it
// gives up (~18 s at HOMING_RPM), and nothing about the next attempt is
// different while the hall sensor or its magnet is still broken. The watchdog
// cannot catch a unit that keeps seeking: the seek feeds it on every step.
//
// So a failed home leaves a retry owed, and the unit pays it by itself — it
// seeks again once a wait has passed, then shows whatever letter is commanded.
// A one-off failure (a glitch, a rail dip mid-seek) heals without the master
// doing anything; a broken sensor makes each consecutive failure double the
// wait. SFP_CMD_HOME is somebody acting on the unit — a master's boot or
// reflash sequence, an operator after a repair — so it waits only the base gap
// however long the automatic one has grown.
//
// The state is RAM only: SFP_CMD_REBOOT (or a power cycle) clears it.

#include <stdint.h>

// Wait after the first failure, measured from the END of the failed seek.
#define HOME_RETRY_BASE_MS    30000UL
// Doublings before the wait stops growing: 30 s << 5 = 16 min. At that wait a
// unit that never finds its marker drives its motor about 2 % of the time,
// which is what a healthy unit showing a clock does.
#define HOME_RETRY_MAX_SHIFT  5

struct HomeBackoff {
  uint8_t consecutiveFails;  // saturating; 0 = the last attempt succeeded
  uint32_t lastFailEndMs;    // millis() when the last failed seek stopped
};

inline uint32_t homeRetryWaitMs(uint8_t consecutiveFails) {
  if (consecutiveFails == 0) return 0;
  uint8_t shift = (uint8_t)(consecutiveFails - 1);
  if (shift > HOME_RETRY_MAX_SHIFT) shift = HOME_RETRY_MAX_SHIFT;
  return (uint32_t)HOME_RETRY_BASE_MS << shift;
}

// The automatic retry, and a letter sent to an unhomed unit: the full,
// doubling wait. Unsigned subtraction keeps the comparison right across the
// millis() wrap.
inline bool homeAttemptAllowed(const HomeBackoff& b, uint32_t nowMs) {
  if (b.consecutiveFails == 0) return true;
  return (uint32_t)(nowMs - b.lastFailEndMs) >=
         homeRetryWaitMs(b.consecutiveFails);
}

// A home has failed and none has succeeded since.
inline bool homeRetryOwed(const HomeBackoff& b) {
  return b.consecutiveFails != 0;
}

// SFP_CMD_HOME: the base wait only.
inline bool homeCommandAllowed(const HomeBackoff& b, uint32_t nowMs) {
  if (b.consecutiveFails == 0) return true;
  return (uint32_t)(nowMs - b.lastFailEndMs) >= HOME_RETRY_BASE_MS;
}

inline void homeNoteResult(HomeBackoff& b, bool found, uint32_t nowMs) {
  if (found) {
    b.consecutiveFails = 0;
    return;
  }
  if (b.consecutiveFails < 0xFF) b.consecutiveFails++;
  b.lastFailEndMs = nowMs;
}
