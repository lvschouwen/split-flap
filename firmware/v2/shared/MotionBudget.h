#pragma once
// Motion admission (#505). The row master, its units and their steppers share
// one 5 V supply. RenderStagger.h (#324) only spreads the START of each move;
// a move lasts seconds, so a full-row render still ends with every stepper
// energised at once, and a broadcast HOME started them all in one transaction.
// This header is the natively-tested policy; the Wire glue and the actual
// polling/delays stay in UnitBus.cpp / FollowerBus.cpp.
//
//   MotionTracker     - at most `cap` units moving at once: a unit is admitted
//                       only when a tracked mover has gone idle
//   MotionBudgetState - the cap itself, halved on each new rail-sag low a unit
//                       reports (vmin, #306), restored one step per quiet period
//   MotionRadioGate   - no motion while the radio is joining or an OTA writes,
//                       plus a settle; closed at boot until the first join resolves
//
// SHARED header: included by Master and FollowerEsp01.

#include <stdint.h>

// Ceiling for concurrently moving units. Overridable per build (the ESP-01
// row runs lower, see its platformio.ini).
#ifndef MOTION_BUDGET_MAX
#define MOTION_BUDGET_MAX 4
#endif
#define MOTION_BUDGET_MIN 1
// A unit vmin below this is a rail-sag event. The Nano's 16 MHz clock needs
// ~4.5 V; the wall idles at ~4.9-5.1 V.
#define MOTION_SAG_LOW_MV 4700
// One restore step per this much time without a new sag event.
#define MOTION_BUDGET_RESTORE_MS 600000UL
// A just-commanded unit counts as moving for this long whatever it reports:
// the sketch picks the command up from loop(), not the TWI ISR.
#define MOTION_START_GRACE_MS 150UL
// A mover still reporting busy after this long is dropped from the count
// (a full failed home is ~18 s at HOMING_RPM); the frame's own stop-wait
// handles it from there.
#define MOTION_STUCK_MS 20000UL
// Motion resumes this long after the radio goes quiet.
#define MOTION_RADIO_SETTLE_MS 2000UL
// Upper bound on one radio hold, so a radio that never settles can't freeze
// the display (a join attempt is 30 s).
#define MOTION_RADIO_MAX_HOLD_MS 35000UL
// Units tracked for sag baselines (a row is at most 16 units).
#define MOTION_UNITS_MAX 16
// Tracker capacity = the ceiling.
#define MOTION_TRACK_MAX MOTION_BUDGET_MAX

// --- concurrency tracker ------------------------------------------------------

struct MotionTracker {
  int8_t unit[MOTION_TRACK_MAX] = {};
  uint32_t startMs[MOTION_TRACK_MAX] = {};
  uint8_t count = 0;
};

inline bool motionTrackerFull(const MotionTracker& t, int cap) {
  if (cap < MOTION_BUDGET_MIN) cap = MOTION_BUDGET_MIN;
  return t.count >= cap;
}

inline void motionTrackerAdd(MotionTracker& t, int unitIndex, uint32_t nowMs) {
  if (t.count >= MOTION_TRACK_MAX) return;
  t.unit[t.count] = (int8_t)unitIndex;
  t.startMs[t.count] = nowMs;
  t.count++;
}

inline void motionTrackerRemove(MotionTracker& t, int k) {
  for (int j = k; j + 1 < t.count; j++) {
    t.unit[j] = t.unit[j + 1];
    t.startMs[j] = t.startMs[j + 1];
  }
  t.count--;
}

// Folds one moving-poll reply for tracked slot k (0 = idle, -1 = no answer,
// anything else = still moving). Returns true if the slot was retired.
inline bool motionTrackerObserve(MotionTracker& t, int k, int reply,
                                 uint32_t nowMs) {
  if (k < 0 || k >= t.count) return false;
  uint32_t age = nowMs - t.startMs[k];
  if (age < MOTION_START_GRACE_MS) return false;
  bool moving = reply != 0 && reply != -1;
  if (moving && age < MOTION_STUCK_MS) return false;
  motionTrackerRemove(t, k);
  return true;
}

// Retires slot k as idle without a poll (tests, and callers that know).
inline bool motionTrackerRetireIdle(MotionTracker& t, int k, uint32_t nowMs) {
  return motionTrackerObserve(t, k, 0, nowMs);
}

// --- sag-adaptive budget --------------------------------------------------------

struct MotionBudgetState {
  uint8_t cap = MOTION_BUDGET_MAX;
  uint32_t lastChangeMs = 0;
  uint16_t sagEvents = 0;                    // since boot, saturating
  uint16_t lastVminMv[MOTION_UNITS_MAX] = {};  // per-unit baseline, 0 = none
};

// Folds one unit's since-boot vmin. Only a NEW low (the unit's vmin dropped
// since last seen) below the threshold is a sag event; a rise means the unit
// rebooted and simply re-baselines. Returns true when the cap was halved.
inline bool motionBudgetObserveVmin(MotionBudgetState& b, int unitIndex,
                                    uint16_t vminMv, uint32_t nowMs) {
  if (vminMv == 0) return false;  // not measured
  if (unitIndex < 0 || unitIndex >= MOTION_UNITS_MAX) return false;
  uint16_t prev = b.lastVminMv[unitIndex];
  b.lastVminMv[unitIndex] = vminMv;
  if (vminMv >= MOTION_SAG_LOW_MV) return false;
  if (prev != 0 && vminMv >= prev) return false;  // same or rebooted-higher low
  if (b.sagEvents < 0xFFFF) b.sagEvents++;
  uint8_t halved = (uint8_t)(b.cap / 2);
  b.cap = halved < MOTION_BUDGET_MIN ? MOTION_BUDGET_MIN : halved;
  b.lastChangeMs = nowMs;
  return true;
}

// Restores one step per quiet MOTION_BUDGET_RESTORE_MS, up to the ceiling.
// Returns true when the cap changed.
inline bool motionBudgetTick(MotionBudgetState& b, uint32_t nowMs) {
  if (b.cap >= MOTION_BUDGET_MAX) return false;
  if (nowMs - b.lastChangeMs < MOTION_BUDGET_RESTORE_MS) return false;
  b.cap++;
  b.lastChangeMs = nowMs;
  return true;
}

// --- radio-quiet gate -------------------------------------------------------------

struct MotionRadioGate {
  bool resolved = false;  // saw the radio quiet at least once since boot
  bool busy = true;
  uint32_t quietSinceMs = 0;
};

inline void motionRadioObserve(MotionRadioGate& g, bool radioBusy,
                               uint32_t nowMs) {
  if (radioBusy) {
    g.busy = true;
    return;
  }
  if (g.busy || !g.resolved) {
    g.busy = false;
    g.resolved = true;
    g.quietSinceMs = nowMs;
  }
}

inline bool motionRadioQuiet(const MotionRadioGate& g, uint32_t nowMs) {
  if (!g.resolved || g.busy) return false;
  return nowMs - g.quietSinceMs >= MOTION_RADIO_SETTLE_MS;
}

inline bool motionRadioHoldExpired(uint32_t holdStartMs, uint32_t nowMs) {
  return nowMs - holdStartMs >= MOTION_RADIO_MAX_HOLD_MS;
}
