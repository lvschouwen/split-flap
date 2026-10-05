// Native tests for UnitRescuePolicy.h (#498): when a lost unit gets a rescue
// attempt, and when a rescued unit gets its frame back.
#include <unity.h>

#include "ReflashPlan.h"
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

// A unit held for crashing counts as an attempt, not as a twiboot exit: it
// was deliberately not started.
static void test_a_crash_held_unit_is_an_attempt_not_an_exit() {
  UnitRescueState r;
  unitRescueNoteAttempt(r, 1000, UnitRescueProbe::CrashHeld);
  TEST_ASSERT_EQUAL_UINT16(1, r.attempts);
  TEST_ASSERT_EQUAL_UINT16(0, r.exits);
}

// --- a unit held in its bootloader for crashing (#542) ----------------------

static UnitFacts heldUnit() {
  UnitFacts u = lostUnit();
  u.i2cErrors = 7;
  u.lastErrorMs = 1234;
  u.rescueExits = 2;
  u.diagValid = true;
  u.physLetter = 5;
  u.resetSeen = true;
  u.healthEventState = 0xFF;
  TwibootIdentity id;
  id.generation = 3;
  id.crashValid = true;
  id.crashCount = TWIBOOT_CRASH_HOLD_COUNT;
  unitFactsBecomeBootloader(u, id);
  return u;
}

static void test_a_held_unit_becomes_a_clean_bootloader_unit() {
  UnitFacts u = heldUnit();
  TEST_ASSERT_EQUAL_UINT8(2, u.state);
  TEST_ASSERT_TRUE(twibootHeldForCrashing(u.bootloader));
  // Everything its application last said is void...
  TEST_ASSERT_FALSE(u.statusValid);
  TEST_ASSERT_FALSE(u.stale);
  TEST_ASSERT_FALSE(u.diagValid);
  TEST_ASSERT_FALSE(u.resetSeen);
  TEST_ASSERT_EQUAL_UINT8(0, u.healthEventState);
  // ...the bus-side history is not.
  TEST_ASSERT_EQUAL_UINT16(7, u.i2cErrors);
  TEST_ASSERT_EQUAL_UINT32(1234, u.lastErrorMs);
  TEST_ASSERT_EQUAL_UINT16(2, u.rescueExits);
  // Faulty, not lost: the rescue stops, the update job takes it.
  TEST_ASSERT_TRUE(unitIsFaultyOrLost(u));
  TEST_ASSERT_FALSE(unitIsLost(u));
  UnitRescueState r;
  TEST_ASSERT_FALSE(unitRescueDue(u, r, 1000));
  uint8_t targets[1];
  TEST_ASSERT_EQUAL(1, reflashCollectFlashTargets(&u, 1, 1, targets));
}

// Nothing polls a bootloader unit, so a held one is looked at again on the
// rescue cadence: a power cycle clears the hold and nobody else would notice.
static void test_a_held_unit_is_rechecked_on_the_rescue_cadence() {
  UnitFacts u = heldUnit();
  UnitRescueState r;
  unitRescueNoteAttempt(r, 1000, UnitRescueProbe::CrashHeld);
  TEST_ASSERT_FALSE(unitHeldRecheckDue(u, r, 1000 + UNIT_RESCUE_RETRY_MS - 1));
  TEST_ASSERT_TRUE(unitHeldRecheckDue(u, r, 1000 + UNIT_RESCUE_RETRY_MS));
  // Only a unit held for crashing: one found in its bootloader by a scan
  // belongs to the reflash path, and a sketch unit to the heartbeat.
  u.bootloader.crashCount = TWIBOOT_CRASH_HOLD_COUNT - 1;
  TEST_ASSERT_FALSE(unitHeldRecheckDue(u, r, 1000 + UNIT_RESCUE_RETRY_MS));
  TEST_ASSERT_FALSE(unitHeldRecheckDue(lostUnit(), r, 1000 + UNIT_RESCUE_RETRY_MS));
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
  RUN_TEST(test_a_crash_held_unit_is_an_attempt_not_an_exit);
  RUN_TEST(test_a_held_unit_becomes_a_clean_bootloader_unit);
  RUN_TEST(test_a_held_unit_is_rechecked_on_the_rescue_cadence);
  return UNITY_END();
}
