// Host-side tests for UnitSupplyWait.h (#505): how long a unit holds a move
// back while its own supply reads low. The rotateToLetter() glue is bench
// tier and pinned by tests/test_unit_motion_glue.py.

#include <unity.h>
#include <stdint.h>
#include "../../UnitSupplyWait.h"

void setUp() {}
void tearDown() {}

static void test_a_healthy_supply_holds_nothing() {
  SupplyWait s;
  TEST_ASSERT_FALSE(supplyWaitHold(s, SUPPLY_WAIT_LOW_MV, 1000));
  TEST_ASSERT_FALSE(supplyWaitHold(s, 5000, 1001));
  TEST_ASSERT_FALSE(supplyWaitTakeHeld(s));
}

static void test_a_low_supply_holds_the_move_until_it_recovers() {
  SupplyWait s;
  TEST_ASSERT_TRUE(supplyWaitHold(s, SUPPLY_WAIT_LOW_MV - 1, 1000));
  TEST_ASSERT_TRUE(supplyWaitHold(s, 4500, 1500));
  TEST_ASSERT_FALSE(supplyWaitHold(s, 4800, 1600));
  TEST_ASSERT_TRUE(supplyWaitTakeHeld(s));
  TEST_ASSERT_FALSE(supplyWaitTakeHeld(s));  // told once per move
}

// A supply that stays low must not park the unit: the move goes after the cap.
static void test_the_hold_is_bounded() {
  SupplyWait s;
  TEST_ASSERT_TRUE(supplyWaitHold(s, 4500, 1000));
  TEST_ASSERT_TRUE(supplyWaitHold(s, 4500, 1000 + SUPPLY_WAIT_MAX_MS - 1));
  TEST_ASSERT_FALSE(supplyWaitHold(s, 4500, 1000 + SUPPLY_WAIT_MAX_MS));
  TEST_ASSERT_TRUE(supplyWaitTakeHeld(s));
  // The next move gets a wait of its own.
  TEST_ASSERT_TRUE(supplyWaitHold(s, 4500, 9000));
  TEST_ASSERT_TRUE(supplyWaitHold(s, 4500, 9000 + SUPPLY_WAIT_MAX_MS - 1));
}

static void test_the_cap_survives_the_millis_wrap() {
  SupplyWait s;
  TEST_ASSERT_TRUE(supplyWaitHold(s, 4500, 0xFFFFFF00UL));
  TEST_ASSERT_TRUE(supplyWaitHold(s, 4500, 0xFFFFFF00UL + SUPPLY_WAIT_MAX_MS - 1));
  TEST_ASSERT_FALSE(supplyWaitHold(s, 4500, 0xFFFFFF00UL + SUPPLY_WAIT_MAX_MS));
}

// 0 is "no reading": a unit that cannot measure moves as it always did.
static void test_no_reading_holds_nothing() {
  SupplyWait s;
  TEST_ASSERT_FALSE(supplyWaitHold(s, 0, 1000));
}

// A letter withdrawn while held: the wait it started is not the next move's.
static void test_a_reset_forgets_the_wait() {
  SupplyWait s;
  TEST_ASSERT_TRUE(supplyWaitHold(s, 4500, 1000));
  supplyWaitReset(s);
  TEST_ASSERT_FALSE(supplyWaitTakeHeld(s));
  TEST_ASSERT_TRUE(supplyWaitHold(s, 4500, 1000 + 10 * SUPPLY_WAIT_MAX_MS));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_a_healthy_supply_holds_nothing);
  RUN_TEST(test_a_low_supply_holds_the_move_until_it_recovers);
  RUN_TEST(test_the_hold_is_bounded);
  RUN_TEST(test_the_cap_survives_the_millis_wrap);
  RUN_TEST(test_no_reading_holds_nothing);
  RUN_TEST(test_a_reset_forgets_the_wait);
  return UNITY_END();
}
