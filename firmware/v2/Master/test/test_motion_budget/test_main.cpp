// Native tests for MotionBudget.h (#505): concurrent-mover cap, sag-adaptive
// budget, radio-quiet gate.
#include <unity.h>

#include "MotionBudget.h"

void setUp() {}
void tearDown() {}

// --- concurrency tracker ------------------------------------------------------

static void test_tracker_fills_to_cap() {
  MotionTracker t;
  TEST_ASSERT_FALSE(motionTrackerFull(t, 2));
  motionTrackerAdd(t, 0, 1000);
  motionTrackerAdd(t, 1, 1000);
  TEST_ASSERT_TRUE(motionTrackerFull(t, 2));
  TEST_ASSERT_FALSE(motionTrackerFull(t, 3));
}

static void test_fresh_mover_is_held_through_start_grace() {
  // A unit just commanded may not report moving yet: count it anyway.
  MotionTracker t;
  motionTrackerAdd(t, 3, 1000);
  motionTrackerRetireIdle(t, 0, 1000 + MOTION_START_GRACE_MS - 1);
  TEST_ASSERT_EQUAL(1, t.count);
  motionTrackerRetireIdle(t, 0, 1000 + MOTION_START_GRACE_MS);
  TEST_ASSERT_EQUAL(0, t.count);
}

static void test_moving_unit_stays_until_idle() {
  MotionTracker t;
  motionTrackerAdd(t, 3, 1000);
  TEST_ASSERT_FALSE(motionTrackerObserve(t, 0, 1, 5000));  // still moving
  TEST_ASSERT_EQUAL(1, t.count);
  TEST_ASSERT_TRUE(motionTrackerObserve(t, 0, 0, 6000));   // idle -> retired
  TEST_ASSERT_EQUAL(0, t.count);
}

static void test_silent_unit_does_not_hold_a_slot() {
  // -1 = no answer: a lost unit must not block the frame.
  MotionTracker t;
  motionTrackerAdd(t, 3, 1000);
  TEST_ASSERT_TRUE(motionTrackerObserve(t, 0, -1, 1000 + MOTION_START_GRACE_MS));
}

static void test_stuck_mover_is_retired() {
  MotionTracker t;
  motionTrackerAdd(t, 3, 1000);
  TEST_ASSERT_FALSE(motionTrackerObserve(t, 0, 1, 1000 + MOTION_STUCK_MS - 1));
  TEST_ASSERT_TRUE(motionTrackerObserve(t, 0, 1, 1000 + MOTION_STUCK_MS));
}

static void test_retire_compacts_the_slot_list() {
  MotionTracker t;
  motionTrackerAdd(t, 4, 1000);
  motionTrackerAdd(t, 5, 1000);
  motionTrackerAdd(t, 6, 1000);
  motionTrackerObserve(t, 1, 0, 1000 + MOTION_START_GRACE_MS);
  TEST_ASSERT_EQUAL(2, t.count);
  TEST_ASSERT_EQUAL(4, t.unit[0]);
  TEST_ASSERT_EQUAL(6, t.unit[1]);
}

static void test_add_beyond_capacity_is_ignored() {
  MotionTracker t;
  for (int i = 0; i < MOTION_TRACK_MAX + 3; i++) motionTrackerAdd(t, i, 1000);
  TEST_ASSERT_EQUAL(MOTION_TRACK_MAX, t.count);
}

// --- sag-adaptive budget --------------------------------------------------------

static void test_budget_starts_at_ceiling() {
  MotionBudgetState b;
  TEST_ASSERT_EQUAL(MOTION_BUDGET_MAX, b.cap);
}

static void test_new_low_below_threshold_halves_the_cap() {
  MotionBudgetState b;
  TEST_ASSERT_TRUE(motionBudgetObserveVmin(b, 2, MOTION_SAG_LOW_MV - 50, 1000));
  TEST_ASSERT_EQUAL(MOTION_BUDGET_MAX / 2, b.cap);
  TEST_ASSERT_EQUAL(1, b.sagEvents);
}

static void test_repeated_same_low_is_one_event() {
  // vmin is the unit's since-boot minimum: re-reading it is not a new sag.
  MotionBudgetState b;
  motionBudgetObserveVmin(b, 2, 4600, 1000);
  TEST_ASSERT_FALSE(motionBudgetObserveVmin(b, 2, 4600, 2000));
  TEST_ASSERT_EQUAL(1, b.sagEvents);
}

static void test_deeper_low_is_another_event() {
  MotionBudgetState b;
  motionBudgetObserveVmin(b, 2, 4600, 1000);
  TEST_ASSERT_TRUE(motionBudgetObserveVmin(b, 2, 4500, 2000));
  TEST_ASSERT_EQUAL(2, b.sagEvents);
}

static void test_healthy_or_unknown_vmin_changes_nothing() {
  MotionBudgetState b;
  TEST_ASSERT_FALSE(motionBudgetObserveVmin(b, 2, 4900, 1000));
  TEST_ASSERT_FALSE(motionBudgetObserveVmin(b, 2, 0, 1000));  // not measured
  TEST_ASSERT_EQUAL(MOTION_BUDGET_MAX, b.cap);
}

static void test_cap_never_drops_below_one() {
  MotionBudgetState b;
  for (int v = 4600; v > 4000; v -= 50) motionBudgetObserveVmin(b, 1, v, 1000);
  TEST_ASSERT_EQUAL(1, b.cap);
}

static void test_unit_reboot_resets_its_baseline() {
  // A rebooted unit's vmin starts high again; its next sag counts anew.
  MotionBudgetState b;
  motionBudgetObserveVmin(b, 2, 4600, 1000);
  motionBudgetObserveVmin(b, 2, 5000, 2000);
  TEST_ASSERT_TRUE(motionBudgetObserveVmin(b, 2, 4650, 3000));
}

static void test_cap_restores_one_step_per_quiet_period() {
  MotionBudgetState b;
  motionBudgetObserveVmin(b, 1, 4600, 1000);
  uint8_t low = b.cap;
  motionBudgetTick(b, 1000 + MOTION_BUDGET_RESTORE_MS - 1);
  TEST_ASSERT_EQUAL(low, b.cap);
  motionBudgetTick(b, 1000 + MOTION_BUDGET_RESTORE_MS);
  TEST_ASSERT_EQUAL(low + 1, b.cap);
  motionBudgetTick(b, 1000 + MOTION_BUDGET_RESTORE_MS + 1);
  TEST_ASSERT_EQUAL(low + 1, b.cap);  // one step per period, not per tick
}

static void test_cap_never_restores_past_ceiling() {
  MotionBudgetState b;
  motionBudgetTick(b, 10UL * MOTION_BUDGET_RESTORE_MS);
  TEST_ASSERT_EQUAL(MOTION_BUDGET_MAX, b.cap);
}

// --- radio-quiet gate -------------------------------------------------------------

static void test_radio_gate_starts_closed() {
  // Boot: the first join has not resolved yet, so no motion.
  MotionRadioGate g;
  TEST_ASSERT_FALSE(motionRadioQuiet(g, 0));
}

static void test_radio_gate_opens_after_settle() {
  MotionRadioGate g;
  motionRadioObserve(g, true, 1000);
  motionRadioObserve(g, false, 1500);
  TEST_ASSERT_FALSE(motionRadioQuiet(g, 1500 + MOTION_RADIO_SETTLE_MS - 1));
  TEST_ASSERT_TRUE(motionRadioQuiet(g, 1500 + MOTION_RADIO_SETTLE_MS));
}

static void test_radio_busy_closes_the_gate() {
  MotionRadioGate g;
  motionRadioObserve(g, false, 1000);
  TEST_ASSERT_TRUE(motionRadioQuiet(g, 1000 + MOTION_RADIO_SETTLE_MS));
  motionRadioObserve(g, true, 9000);
  TEST_ASSERT_FALSE(motionRadioQuiet(g, 9001));
}

static void test_hold_is_bounded() {
  // A radio that never settles must not freeze the display forever.
  TEST_ASSERT_FALSE(motionRadioHoldExpired(1000, 1000 + MOTION_RADIO_MAX_HOLD_MS - 1));
  TEST_ASSERT_TRUE(motionRadioHoldExpired(1000, 1000 + MOTION_RADIO_MAX_HOLD_MS));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_tracker_fills_to_cap);
  RUN_TEST(test_fresh_mover_is_held_through_start_grace);
  RUN_TEST(test_moving_unit_stays_until_idle);
  RUN_TEST(test_silent_unit_does_not_hold_a_slot);
  RUN_TEST(test_stuck_mover_is_retired);
  RUN_TEST(test_retire_compacts_the_slot_list);
  RUN_TEST(test_add_beyond_capacity_is_ignored);
  RUN_TEST(test_budget_starts_at_ceiling);
  RUN_TEST(test_new_low_below_threshold_halves_the_cap);
  RUN_TEST(test_repeated_same_low_is_one_event);
  RUN_TEST(test_deeper_low_is_another_event);
  RUN_TEST(test_healthy_or_unknown_vmin_changes_nothing);
  RUN_TEST(test_cap_never_drops_below_one);
  RUN_TEST(test_unit_reboot_resets_its_baseline);
  RUN_TEST(test_cap_restores_one_step_per_quiet_period);
  RUN_TEST(test_cap_never_restores_past_ceiling);
  RUN_TEST(test_radio_gate_starts_closed);
  RUN_TEST(test_radio_gate_opens_after_settle);
  RUN_TEST(test_radio_busy_closes_the_gate);
  RUN_TEST(test_hold_is_bounded);
  return UNITY_END();
}
