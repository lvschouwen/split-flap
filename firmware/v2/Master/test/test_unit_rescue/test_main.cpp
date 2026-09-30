// Native tests for UnitRescuePolicy.h (#498): when a lost unit gets a rescue
// attempt, and when a rescued unit gets its frame back.
#include <unity.h>

#include "UnitRescuePolicy.h"

void setUp() {}
void tearDown() {}

static UnitFacts lostUnit() {
  UnitFacts u;
  u.state = 1;
  u.stale = true;
  return u;
}

static UnitFacts answeringUnit() {
  UnitFacts u;
  u.state = 1;
  u.statusValid = true;
  return u;
}

static void test_healthy_unit_is_never_due() {
  UnitRescueState r;
  TEST_ASSERT_FALSE(unitRescueDue(answeringUnit(), r, 1000));
}

static void test_lost_unit_is_due_immediately() {
  UnitRescueState r;
  TEST_ASSERT_TRUE(unitRescueDue(lostUnit(), r, 1000));
}

static void test_non_sketch_slots_are_never_due() {
  UnitRescueState r;
  UnitFacts u = lostUnit();
  u.state = 2;  // already known to be in twiboot: the reflash path owns it
  TEST_ASSERT_FALSE(unitRescueDue(u, r, 1000));
  u.state = 0;
  TEST_ASSERT_FALSE(unitRescueDue(u, r, 1000));
}

static void test_retry_is_rate_limited() {
  UnitRescueState r;
  unitRescueNoteAttempt(r, 1000, UnitRescueProbe::NoAck);
  TEST_ASSERT_FALSE(unitRescueDue(lostUnit(), r, 1000 + UNIT_RESCUE_RETRY_MS - 1));
  TEST_ASSERT_TRUE(unitRescueDue(lostUnit(), r, 1000 + UNIT_RESCUE_RETRY_MS));
}

static void test_retry_survives_millis_wrap() {
  UnitRescueState r;
  unitRescueNoteAttempt(r, 0xFFFFFF00UL, UnitRescueProbe::NoAck);
  TEST_ASSERT_FALSE(unitRescueDue(lostUnit(), r, 0x00000010UL));
  TEST_ASSERT_TRUE(unitRescueDue(lostUnit(), r,
                                 0xFFFFFF00UL + UNIT_RESCUE_RETRY_MS));
}

static void test_bootloader_exit_counts_and_arms_restore() {
  UnitRescueState r;
  unitRescueNoteAttempt(r, 1000, UnitRescueProbe::Bootloader);
  TEST_ASSERT_EQUAL_UINT16(1, r.exits);
  TEST_ASSERT_EQUAL_UINT16(1, r.attempts);
  TEST_ASSERT_TRUE(r.restorePending);
  // Still lost: no restore yet.
  TEST_ASSERT_FALSE(unitRescueObserve(r, lostUnit()));
  // First good read after the exit: restore exactly once.
  TEST_ASSERT_TRUE(unitRescueObserve(r, answeringUnit()));
  TEST_ASSERT_FALSE(unitRescueObserve(r, answeringUnit()));
}

static void test_any_recovery_from_loss_restores_once() {
  // A unit that power-cycled on its own (unplugged, brownout) comes back
  // unhomed and blank too: every loss episode ends in one re-show.
  UnitRescueState r;
  unitRescueNoteAttempt(r, 1000, UnitRescueProbe::NoAck);
  unitRescueNoteAttempt(r, 70000, UnitRescueProbe::SketchSilent);
  TEST_ASSERT_EQUAL_UINT16(0, r.exits);
  TEST_ASSERT_EQUAL_UINT16(2, r.attempts);
  TEST_ASSERT_TRUE(unitRescueObserve(r, answeringUnit()));
  TEST_ASSERT_FALSE(unitRescueObserve(r, answeringUnit()));
}

static void test_recovery_rearms_immediate_rescue() {
  // A unit that comes back by itself and is lost again later gets an
  // attempt straight away, not after the previous episode's backoff.
  UnitRescueState r;
  unitRescueNoteAttempt(r, 1000, UnitRescueProbe::NoAck);
  unitRescueObserve(r, answeringUnit());
  TEST_ASSERT_TRUE(unitRescueDue(lostUnit(), r, 2000));
}

static void test_counters_saturate() {
  UnitRescueState r;
  r.exits = 0xFFFF;
  r.attempts = 0xFFFF;
  unitRescueNoteAttempt(r, 1000, UnitRescueProbe::Bootloader);
  TEST_ASSERT_EQUAL_UINT16(0xFFFF, r.exits);
  TEST_ASSERT_EQUAL_UINT16(0xFFFF, r.attempts);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_healthy_unit_is_never_due);
  RUN_TEST(test_lost_unit_is_due_immediately);
  RUN_TEST(test_non_sketch_slots_are_never_due);
  RUN_TEST(test_retry_is_rate_limited);
  RUN_TEST(test_retry_survives_millis_wrap);
  RUN_TEST(test_bootloader_exit_counts_and_arms_restore);
  RUN_TEST(test_any_recovery_from_loss_restores_once);
  RUN_TEST(test_recovery_rearms_immediate_rescue);
  RUN_TEST(test_counters_saturate);
  return UNITY_END();
}
