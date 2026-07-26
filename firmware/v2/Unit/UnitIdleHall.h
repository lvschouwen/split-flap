#pragma once
// Pure idle hall-consistency logic (#268), natively tested by test_idle_hall.
// The pin sampling, the suppression conditions and the re-home this arms are
// .ino glue (bench tier).
//
// THE PROBLEM. The hall sensor is sampled only while the drum is stepping, so
// a drum turned by hand at rest is invisible: the unit keeps believing it
// shows what it last commanded until some later move happens to sweep the
// magnet past the sensor. Until then the wall is wrong and every health field
// says it is fine.
//
// THE ONE BIT AT REST. #263 already tracks drumPosition — steps past the hall
// entering edge, resynced to 0 at every observed edge. So the unit can PREDICT
// the sensor: a parked position inside the magnet window must read low, one
// outside it must read high. A contradiction that survives the debounce is a
// drum that moved without us, and the existing idle re-home restores the
// letter.
//
// COVERAGE IS PARTIAL BY PHYSICS, and smaller than #268 estimated. The window
// is ~46 steps of 2038 (~one flap), and this wall's calibration offsets are
// 44..89 — so blank parks OUTSIDE the window on every unit but one, contrary
// to the issue's premise. What that leaves:
//   believed outside (nearly every park) — caught only if the disturbance
//       lands the drum inside that ~2.3% slice of the revolution
//   believed inside (the one letter per unit parking in-window) — almost any
//       nudge leaves the window, so nearly always caught
// #269's scheduled verification re-home is what covers the rest; this is the
// free half that costs one digitalRead every 250 ms.
//
// TWO CLASSES OF FALSE POSITIVE, AND WHY ONLY ONE IS SELF-LIMITING.
//   STALE BELIEF — the re-home resyncs the position to the physical edge, so
//       the corrected belief agrees with the sensor and the alarm cannot
//       repeat. This one really is self-limiting.
//   WRONG WINDOW — the re-home cannot touch it. calibrate() re-parks the drum
//       at the same physical spot and the model still disagrees, so the check
//       re-arms every cooldown for the life of the unit.
// An earlier version of this header claimed the two-tier margin guarded the
// second case. It does not, and cannot: the margin bounds how far a reading
// may be off, while the dangerous input is a reading that is simply no longer
// true. `selfTestLastHallWindow` is a HISTORICAL record that only moves when
// someone re-runs the self-test, so a unit whose magnet weakened still reports
// the width it had when healthy — which is exactly the decline #406 exists to
// record. Hence two guards below that do not depend on the margin at all: a
// plausibility band on the input, and idleHallStandDown() on the outcome.

#include <stdint.h>

#define IDLE_HALL_EXPECT_UNKNOWN 0  // too close to an edge to judge
#define IDLE_HALL_EXPECT_LOW     1  // parked inside the magnet window
#define IDLE_HALL_EXPECT_HIGH    2  // parked outside it

// Fallback window for a unit that has never run a #265 self-test. 46 steps is
// a15's healthy measurement; every other unit's is unknown until it measures
// its own.
#define IDLE_HALL_WINDOW_ASSUMED_STEPS   46

// A measured window is only trusted inside this band; outside it the assumed
// pair is used instead. Both ends are load-bearing, and neither is arbitrary:
//
//   TOO WIDE asserts a magnet where there is none. A drum that binds inside
//   the window during the self-test keeps counting hall-low samples, so a
//   several-hundred-step "window" is recordable. At margin 12 that claims LOW
//   across most of the revolution, and every park outside the real ~46 steps
//   contradicts forever.
//   TOO NARROW asserts the opposite. The self-test's phase-1 approach exits on
//   a single un-debounced low sample, so one glitch seeds a 1-step window —
//   which claims HIGH from position 13 up, while a drum parked inside the real
//   magnet reads LOW.
//
// A hall window is one magnet passing one sensor: a couple of flaps wide at
// most. Anything outside that is a broken measurement, not a narrow drum.
#define IDLE_HALL_WINDOW_TRUST_MIN_STEPS 23
#define IDLE_HALL_WINDOW_TRUST_MAX_STEPS 127

// Steps either side of a window edge where no verdict is issued. Two tiers,
// because the two window sources carry different uncertainty:
//   MEASURED — this unit's own reading. What remains is the ~2-step
//       disagreement between the observers that define "the edge" (calibrate
//       accepts the first low sample, stepFlaps debounces with two) plus the
//       sub-threshold belief error #263 tolerates without calling it drift.
//   ASSUMED — carries the unknown per-unit spread on top of that. Deliberately
//       wide enough that it meets itself across the assumed 46-step window, so
//       an unmeasured unit NEVER asserts "there must be a magnet here" against
//       a guess. It still polices the far side, which is where nearly every
//       unit actually parks.
#define IDLE_HALL_MARGIN_MEASURED_STEPS  12
#define IDLE_HALL_MARGIN_ASSUMED_STEPS   24

// Consecutive contradicting samples before the disturbance is believed, and
// how often they are taken. 6 x 250 ms = 1.5 s of persistent disagreement —
// long enough that a single noisy read costs nothing, short enough that a
// hand-turned drum heals while you are still standing at the wall.
#define IDLE_HALL_CONFIRM_SAMPLES        6
#define IDLE_HALL_SAMPLE_INTERVAL_MS     250

struct IdleHallWindow {
  uint16_t steps;   // magnet window width, from the entering edge
  uint16_t margin;  // unjudgeable band either side of each edge
};

// Which window model to police against. A measurement is trusted only inside
// the plausibility band above; 0 (never measured) and anything outside it fall
// back to the assumed pair, whose wide margin asserts nothing it cannot back.
//
// The earlier version trusted any width from 1 to a quarter-revolution on the
// reasoning that a degrading sensor genuinely measures a smaller window. That
// reasoning was right about the physics and wrong about the input: the stored
// value is a HISTORICAL record (#406) that only changes when someone re-runs
// the self-test, so it is stale by construction — the unit whose window shrank
// still reports the width it had when healthy. Trusting a stale reading with
// the tight margin is what turns this check into a re-home loop, which is why
// the caller also disarms on futile re-homes: the band narrows the input, the
// disarm bounds every cause the band cannot see.
inline IdleHallWindow idleHallResolveWindow(uint16_t measuredSteps,
                                            uint16_t stepsPerRev) {
  IdleHallWindow w;
  (void)stepsPerRev;
  if (measuredSteps < IDLE_HALL_WINDOW_TRUST_MIN_STEPS ||
      measuredSteps > IDLE_HALL_WINDOW_TRUST_MAX_STEPS) {
    w.steps = IDLE_HALL_WINDOW_ASSUMED_STEPS;
    w.margin = IDLE_HALL_MARGIN_ASSUMED_STEPS;
    return w;
  }
  w.steps = measuredSteps;
  w.margin = IDLE_HALL_MARGIN_MEASURED_STEPS;
  return w;
}

// A re-home this check armed either measured a real drift deviation or it did
// not. One that did not is proof the WINDOW model was wrong, not the belief —
// calibrate() resyncs to the physical edge and re-parks at the same place, so
// the same contradiction returns and the detector re-arms every cooldown for
// the life of the unit. Count those and stand down.
//
// This is the backstop the module's original "a false positive is
// self-limiting" claim was missing: that holds for stale-belief alarms, which
// the re-home genuinely corrects, and not for a wrong window, which it cannot
// touch. Deliberately a since-boot RAM counter — a power cycle is a free
// retry, and a re-run self-test that fixes the window comes with one.
#define IDLE_HALL_FUTILE_REHOME_LIMIT 3

inline bool idleHallStandDown(uint8_t futileRehomes) {
  return futileRehomes >= IDLE_HALL_FUTILE_REHOME_LIMIT;
}

// What the sensor should read with the drum parked `position` steps past the
// entering edge. Both bands are inclusive of their far end and exclude the
// margin around both edges — the entering edge at 0 (approached from either
// side of the wrap) and the releasing edge at `steps`. A band the margins
// swallow entirely simply yields no verdict, which is the intended answer,
// not a degenerate case.
inline uint8_t idleHallExpectation(uint16_t position, const IdleHallWindow& w,
                                   uint16_t stepsPerRev) {
  if (position >= stepsPerRev) return IDLE_HALL_EXPECT_UNKNOWN;
  if (w.steps > w.margin && position >= w.margin &&
      position + w.margin < w.steps) {
    return IDLE_HALL_EXPECT_LOW;
  }
  if (position >= (uint16_t)(w.steps + w.margin) &&
      position + w.margin < stepsPerRev) {
    return IDLE_HALL_EXPECT_HIGH;
  }
  return IDLE_HALL_EXPECT_UNKNOWN;
}

// Debounce state. `expectation` is what the current streak is contradicting:
// the expectation can only change by the drum moving, which is exactly the
// event that invalidates a streak built against the old one.
struct IdleHallCheck {
  uint8_t streak;
  uint8_t expectation;
};

inline void idleHallReset(IdleHallCheck& s) {
  s.streak = 0;
  s.expectation = IDLE_HALL_EXPECT_UNKNOWN;
}

// Fold one idle sample. True means the contradiction has persisted long
// enough to be believed — the caller arms the re-home, and the state clears
// so one disturbance arms exactly one.
inline bool idleHallObserve(IdleHallCheck& s, uint8_t expectation,
                            bool hallLow) {
  bool contradicted = (expectation == IDLE_HALL_EXPECT_LOW && !hallLow) ||
                      (expectation == IDLE_HALL_EXPECT_HIGH && hallLow);
  if (!contradicted) {
    idleHallReset(s);
    return false;
  }
  if (s.expectation != expectation) {
    s.expectation = expectation;
    s.streak = 0;
  }
  s.streak++;
  if (s.streak < IDLE_HALL_CONFIRM_SAMPLES) return false;
  idleHallReset(s);
  return true;
}
