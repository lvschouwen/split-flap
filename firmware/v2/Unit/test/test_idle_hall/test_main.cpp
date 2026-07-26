// Host-side unit tests for the pure idle hall-consistency logic in
// UnitIdleHall.h (#268): resolving which hall-window model to trust, turning
// a parked drum position into an expected sensor reading, and the debounce
// that separates a real disturbance from one noisy sample. The hall sampling
// and the re-home it arms are .ino glue, bench tier.

#include <unity.h>
#include <stdint.h>
#include "../../UnitIdleHall.h"

// The unit's real constants (Unit.ino STEPS) and a1..a16's measured hall
// window (a15's healthy self-test, 2026-07-26).
static const uint16_t kStepsPerRev = 2038;
static const uint16_t kMeasuredWindow = 46;

void setUp() {}
void tearDown() {}

// --- window resolution ------------------------------------------------------

static void test_unmeasured_window_falls_back_to_the_assumed_constant() {
  IdleHallWindow w = idleHallResolveWindow(0, kStepsPerRev);
  TEST_ASSERT_EQUAL_UINT16(IDLE_HALL_WINDOW_ASSUMED_STEPS, w.steps);
  TEST_ASSERT_EQUAL_UINT16(IDLE_HALL_MARGIN_ASSUMED_STEPS, w.margin);
}

static void test_measured_window_is_trusted_with_the_tighter_margin() {
  IdleHallWindow w = idleHallResolveWindow(kMeasuredWindow, kStepsPerRev);
  TEST_ASSERT_EQUAL_UINT16(kMeasuredWindow, w.steps);
  TEST_ASSERT_EQUAL_UINT16(IDLE_HALL_MARGIN_MEASURED_STEPS, w.margin);
}

// A degrading sensor measures a SMALLER window, and that is exactly the
// reading we want to adopt — the fallback would then assert a magnet where
// there is none. It must be trusted all the way down.
static void test_a_shrunken_measured_window_is_still_trusted() {
  IdleHallWindow w = idleHallResolveWindow(12, kStepsPerRev);
  TEST_ASSERT_EQUAL_UINT16(12, w.steps);
  TEST_ASSERT_EQUAL_UINT16(IDLE_HALL_MARGIN_MEASURED_STEPS, w.margin);
}

// A window spanning a quarter of the drum is not a hall window, it is a
// broken measurement — it must not become the model.
static void test_implausibly_wide_measurement_is_rejected() {
  IdleHallWindow w = idleHallResolveWindow(600, kStepsPerRev);
  TEST_ASSERT_EQUAL_UINT16(IDLE_HALL_WINDOW_ASSUMED_STEPS, w.steps);
  TEST_ASSERT_EQUAL_UINT16(IDLE_HALL_MARGIN_ASSUMED_STEPS, w.margin);
}

// --- expectation from a parked position -------------------------------------

static void test_parked_well_inside_the_window_expects_a_low_reading() {
  IdleHallWindow w = idleHallResolveWindow(100, kStepsPerRev);  // margin 12
  TEST_ASSERT_EQUAL_UINT8(IDLE_HALL_EXPECT_LOW,
                          idleHallExpectation(50, w, kStepsPerRev));
}

static void test_parked_well_outside_the_window_expects_a_high_reading() {
  IdleHallWindow w = idleHallResolveWindow(kMeasuredWindow, kStepsPerRev);
  TEST_ASSERT_EQUAL_UINT8(IDLE_HALL_EXPECT_HIGH,
                          idleHallExpectation(1000, w, kStepsPerRev));
}

// Position 0 IS the entering edge. The two observers that resync it (
// calibrate's first low sample, stepFlaps' 2-sample debounce) disagree by a
// step or two about where exactly that is, so the edge itself is never a
// place to judge from.
static void test_the_entering_edge_itself_is_unjudgeable() {
  IdleHallWindow w = idleHallResolveWindow(100, kStepsPerRev);
  TEST_ASSERT_EQUAL_UINT8(IDLE_HALL_EXPECT_UNKNOWN,
                          idleHallExpectation(0, w, kStepsPerRev));
  TEST_ASSERT_EQUAL_UINT8(IDLE_HALL_EXPECT_UNKNOWN,
                          idleHallExpectation(11, w, kStepsPerRev));
  TEST_ASSERT_EQUAL_UINT8(IDLE_HALL_EXPECT_LOW,
                          idleHallExpectation(12, w, kStepsPerRev));
}

static void test_the_releasing_edge_is_unjudgeable_from_both_sides() {
  IdleHallWindow w = idleHallResolveWindow(100, kStepsPerRev);  // margin 12
  TEST_ASSERT_EQUAL_UINT8(IDLE_HALL_EXPECT_LOW,
                          idleHallExpectation(87, w, kStepsPerRev));
  TEST_ASSERT_EQUAL_UINT8(IDLE_HALL_EXPECT_UNKNOWN,
                          idleHallExpectation(88, w, kStepsPerRev));
  TEST_ASSERT_EQUAL_UINT8(IDLE_HALL_EXPECT_UNKNOWN,
                          idleHallExpectation(111, w, kStepsPerRev));
  TEST_ASSERT_EQUAL_UINT8(IDLE_HALL_EXPECT_HIGH,
                          idleHallExpectation(112, w, kStepsPerRev));
}

// The far end of the revolution is the entering edge approached from behind —
// same unjudgeable band, wrapped.
static void test_the_approach_to_the_edge_is_unjudgeable() {
  IdleHallWindow w = idleHallResolveWindow(100, kStepsPerRev);
  TEST_ASSERT_EQUAL_UINT8(IDLE_HALL_EXPECT_HIGH,
                          idleHallExpectation(kStepsPerRev - 13, w,
                                              kStepsPerRev));
  TEST_ASSERT_EQUAL_UINT8(IDLE_HALL_EXPECT_UNKNOWN,
                          idleHallExpectation(kStepsPerRev - 12, w,
                                              kStepsPerRev));
  TEST_ASSERT_EQUAL_UINT8(IDLE_HALL_EXPECT_UNKNOWN,
                          idleHallExpectation(kStepsPerRev - 1, w,
                                              kStepsPerRev));
}

// A position past one revolution is a caller bug, not a verdict.
static void test_an_out_of_range_position_yields_no_verdict() {
  IdleHallWindow w = idleHallResolveWindow(100, kStepsPerRev);
  TEST_ASSERT_EQUAL_UINT8(IDLE_HALL_EXPECT_UNKNOWN,
                          idleHallExpectation(kStepsPerRev, w, kStepsPerRev));
  TEST_ASSERT_EQUAL_UINT8(IDLE_HALL_EXPECT_UNKNOWN,
                          idleHallExpectation(9000, w, kStepsPerRev));
}

// The whole point of the two-tier margin: on the assumed 46-step window the
// margins meet in the middle, so NOTHING inside it is judged. An assumed
// window carries the unknown per-unit spread, and asserting "there must be a
// magnet here" against a guess is what would flap the detector forever.
static void test_the_assumed_window_asserts_no_in_window_position() {
  IdleHallWindow w = idleHallResolveWindow(0, kStepsPerRev);
  for (uint16_t pos = 0; pos < IDLE_HALL_WINDOW_ASSUMED_STEPS; pos++) {
    TEST_ASSERT_EQUAL_UINT8(IDLE_HALL_EXPECT_UNKNOWN,
                            idleHallExpectation(pos, w, kStepsPerRev));
  }
  // It still polices the far side, which is where nearly every unit parks.
  TEST_ASSERT_EQUAL_UINT8(IDLE_HALL_EXPECT_HIGH,
                          idleHallExpectation(500, w, kStepsPerRev));
}

// --- debounce ---------------------------------------------------------------

static IdleHallCheck freshCheck() {
  IdleHallCheck s;
  idleHallReset(s);
  return s;
}

static void test_an_agreeing_sample_never_fires() {
  IdleHallCheck s = freshCheck();
  for (int i = 0; i < IDLE_HALL_CONFIRM_SAMPLES * 3; i++) {
    TEST_ASSERT_FALSE(idleHallObserve(s, IDLE_HALL_EXPECT_HIGH, false));
    TEST_ASSERT_FALSE(idleHallObserve(s, IDLE_HALL_EXPECT_LOW, true));
  }
}

static void test_a_contradiction_fires_only_after_the_full_streak() {
  IdleHallCheck s = freshCheck();
  for (int i = 0; i < IDLE_HALL_CONFIRM_SAMPLES - 1; i++) {
    TEST_ASSERT_FALSE(idleHallObserve(s, IDLE_HALL_EXPECT_HIGH, true));
  }
  TEST_ASSERT_TRUE(idleHallObserve(s, IDLE_HALL_EXPECT_HIGH, true));
}

static void test_a_missing_magnet_where_one_is_expected_also_fires() {
  IdleHallCheck s = freshCheck();
  bool fired = false;
  for (int i = 0; i < IDLE_HALL_CONFIRM_SAMPLES; i++) {
    fired = idleHallObserve(s, IDLE_HALL_EXPECT_LOW, false);
  }
  TEST_ASSERT_TRUE(fired);
}

// One agreeing sample means the drum is where we think it is — a streak that
// survived it would be counting noise, not a disturbance.
static void test_one_agreeing_sample_restarts_the_streak() {
  IdleHallCheck s = freshCheck();
  for (int i = 0; i < IDLE_HALL_CONFIRM_SAMPLES - 1; i++) {
    TEST_ASSERT_FALSE(idleHallObserve(s, IDLE_HALL_EXPECT_HIGH, true));
  }
  TEST_ASSERT_FALSE(idleHallObserve(s, IDLE_HALL_EXPECT_HIGH, false));
  for (int i = 0; i < IDLE_HALL_CONFIRM_SAMPLES - 1; i++) {
    TEST_ASSERT_FALSE(idleHallObserve(s, IDLE_HALL_EXPECT_HIGH, true));
  }
  TEST_ASSERT_TRUE(idleHallObserve(s, IDLE_HALL_EXPECT_HIGH, true));
}

static void test_an_unjudgeable_position_restarts_the_streak() {
  IdleHallCheck s = freshCheck();
  for (int i = 0; i < IDLE_HALL_CONFIRM_SAMPLES - 1; i++) {
    TEST_ASSERT_FALSE(idleHallObserve(s, IDLE_HALL_EXPECT_HIGH, true));
  }
  TEST_ASSERT_FALSE(idleHallObserve(s, IDLE_HALL_EXPECT_UNKNOWN, true));
  TEST_ASSERT_FALSE(idleHallObserve(s, IDLE_HALL_EXPECT_HIGH, true));
}

// The expectation can only change by the drum moving, which is exactly the
// event that invalidates a streak built against the old one.
static void test_a_changed_expectation_restarts_the_streak() {
  IdleHallCheck s = freshCheck();
  for (int i = 0; i < IDLE_HALL_CONFIRM_SAMPLES - 1; i++) {
    TEST_ASSERT_FALSE(idleHallObserve(s, IDLE_HALL_EXPECT_HIGH, true));
  }
  TEST_ASSERT_FALSE(idleHallObserve(s, IDLE_HALL_EXPECT_LOW, false));
}

// Firing arms one re-home. Without a reset the next sample fires again and
// the cooldown becomes the only thing standing between a stuck sensor and a
// re-home every minute forever.
static void test_firing_clears_the_state() {
  IdleHallCheck s = freshCheck();
  bool fired = false;
  for (int i = 0; i < IDLE_HALL_CONFIRM_SAMPLES; i++) {
    fired = idleHallObserve(s, IDLE_HALL_EXPECT_HIGH, true);
  }
  TEST_ASSERT_TRUE(fired);
  TEST_ASSERT_FALSE(idleHallObserve(s, IDLE_HALL_EXPECT_HIGH, true));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_unmeasured_window_falls_back_to_the_assumed_constant);
  RUN_TEST(test_measured_window_is_trusted_with_the_tighter_margin);
  RUN_TEST(test_a_shrunken_measured_window_is_still_trusted);
  RUN_TEST(test_implausibly_wide_measurement_is_rejected);
  RUN_TEST(test_parked_well_inside_the_window_expects_a_low_reading);
  RUN_TEST(test_parked_well_outside_the_window_expects_a_high_reading);
  RUN_TEST(test_the_entering_edge_itself_is_unjudgeable);
  RUN_TEST(test_the_releasing_edge_is_unjudgeable_from_both_sides);
  RUN_TEST(test_the_approach_to_the_edge_is_unjudgeable);
  RUN_TEST(test_an_out_of_range_position_yields_no_verdict);
  RUN_TEST(test_the_assumed_window_asserts_no_in_window_position);
  RUN_TEST(test_an_agreeing_sample_never_fires);
  RUN_TEST(test_a_contradiction_fires_only_after_the_full_streak);
  RUN_TEST(test_a_missing_magnet_where_one_is_expected_also_fires);
  RUN_TEST(test_one_agreeing_sample_restarts_the_streak);
  RUN_TEST(test_an_unjudgeable_position_restarts_the_streak);
  RUN_TEST(test_a_changed_expectation_restarts_the_streak);
  RUN_TEST(test_firing_clears_the_state);
  return UNITY_END();
}
