// Host-side tests for UnitSupplyWait.h (#505): how long a unit holds a move
// back while its own supply reads low against its idle level. The
// rotateToLetter() glue is bench tier and pinned by
// tests/test_unit_motion_glue.py.

#include <unity.h>
#include <stdint.h>
#include "../../UnitSupplyWait.h"

void setUp() {}
void tearDown() {}

#define IDLE 5000
#define LOW (IDLE - SUPPLY_WAIT_DROP_MV - 1)

static void test_a_healthy_supply_holds_nothing() {
  SupplyWait s;
  TEST_ASSERT_FALSE(supplyWaitHold(s, IDLE - SUPPLY_WAIT_DROP_MV, IDLE, 1000));
  TEST_ASSERT_FALSE(supplyWaitHold(s, IDLE, IDLE, 1001));
  TEST_ASSERT_FALSE(supplyWaitTakeHeld(s));
}

static void test_a_low_supply_holds_the_move_until_it_recovers() {
  SupplyWait s;
  TEST_ASSERT_TRUE(supplyWaitHold(s, LOW, IDLE, 1000));
  TEST_ASSERT_TRUE(supplyWaitHold(s, LOW - 200, IDLE, 1500));
  TEST_ASSERT_FALSE(supplyWaitHold(s, IDLE - 100, IDLE, 1600));
  TEST_ASSERT_TRUE(supplyWaitTakeHeld(s));
  TEST_ASSERT_FALSE(supplyWaitTakeHeld(s));  // told once per move
}

// The line follows the chip, not a voltage: a chip that reads the rail low
// and one that reads it high hold at the same dip.
static void test_the_line_is_each_units_own() {
  SupplyWait lowReader, highReader;
  // 4.87 V and 5.16 V at rest on one rail; both see it 0.2 V down.
  TEST_ASSERT_FALSE(supplyWaitHold(lowReader, 4670, 4870, 1000));
  TEST_ASSERT_FALSE(supplyWaitHold(highReader, 4960, 5160, 1000));
  // ... and 0.35 V down.
  TEST_ASSERT_TRUE(supplyWaitHold(lowReader, 4520, 4870, 1000));
  TEST_ASSERT_TRUE(supplyWaitHold(highReader, 4810, 5160, 1000));
}

// A supply that stays low must not park the unit: the move goes after the cap.
static void test_the_hold_is_bounded() {
  SupplyWait s;
  TEST_ASSERT_TRUE(supplyWaitHold(s, LOW, IDLE, 1000));
  TEST_ASSERT_TRUE(supplyWaitHold(s, LOW, IDLE, 1000 + SUPPLY_WAIT_MAX_MS - 1));
  TEST_ASSERT_FALSE(supplyWaitHold(s, LOW, IDLE, 1000 + SUPPLY_WAIT_MAX_MS));
  TEST_ASSERT_TRUE(supplyWaitTakeHeld(s));
  // The next move gets a wait of its own.
  TEST_ASSERT_TRUE(supplyWaitHold(s, LOW, IDLE, 9000));
  TEST_ASSERT_TRUE(supplyWaitHold(s, LOW, IDLE, 9000 + SUPPLY_WAIT_MAX_MS - 1));
}

static void test_the_cap_survives_the_millis_wrap() {
  SupplyWait s;
  TEST_ASSERT_TRUE(supplyWaitHold(s, LOW, IDLE, 0xFFFFFF00UL));
  TEST_ASSERT_TRUE(supplyWaitHold(s, LOW, IDLE, 0xFFFFFF00UL + SUPPLY_WAIT_MAX_MS - 1));
  TEST_ASSERT_FALSE(supplyWaitHold(s, LOW, IDLE, 0xFFFFFF00UL + SUPPLY_WAIT_MAX_MS));
}

// 0 is "no reading" or "no idle level yet": the unit moves as it always did.
static void test_nothing_to_judge_by_holds_nothing() {
  SupplyWait s;
  TEST_ASSERT_FALSE(supplyWaitHold(s, 0, IDLE, 1000));
  TEST_ASSERT_FALSE(supplyWaitHold(s, 3000, 0, 1000));
}

// A letter withdrawn while held: the wait it started is not the next move's.
static void test_a_reset_forgets_the_wait() {
  SupplyWait s;
  TEST_ASSERT_TRUE(supplyWaitHold(s, LOW, IDLE, 1000));
  supplyWaitReset(s);
  TEST_ASSERT_FALSE(supplyWaitTakeHeld(s));
  TEST_ASSERT_TRUE(supplyWaitHold(s, LOW, IDLE, 1000 + 10 * SUPPLY_WAIT_MAX_MS));
}

// --- the idle level ---------------------------------------------------------------

static void test_the_idle_level_takes_a_higher_reading_at_once() {
  uint16_t level = 0;
  supplyIdleLevelFold(level, 4900);
  TEST_ASSERT_EQUAL_UINT16(4900, level);
  supplyIdleLevelFold(level, 5010);
  TEST_ASSERT_EQUAL_UINT16(5010, level);
  supplyIdleLevelFold(level, 0);  // no reading
  TEST_ASSERT_EQUAL_UINT16(5010, level);
}

// A neighbour's full turn is about 5 s: five low readings barely move it, so
// the dip still reads as one.
static void test_a_neighbours_move_does_not_pull_the_level_down() {
  uint16_t level = 5000;
  for (int i = 0; i < 5; i++) supplyIdleLevelFold(level, 4600);
  TEST_ASSERT_TRUE(level >= 4970);
  TEST_ASSERT_TRUE(supplyReadsLow(4600, level));
}

// A rail that settled lower for good: after a few minutes it is the level,
// and moves are no longer held.
static void test_a_rail_that_stays_lower_becomes_the_level() {
  uint16_t level = 5000;
  int readings = 0;
  while (supplyReadsLow(4600, level) && readings < 3600) {
    supplyIdleLevelFold(level, 4600);
    readings++;
  }
  TEST_ASSERT_TRUE(readings > 30);    // not within half a minute
  TEST_ASSERT_TRUE(readings < 300);   // within five
  for (int i = 0; i < 3600; i++) supplyIdleLevelFold(level, 4600);
  TEST_ASSERT_EQUAL_UINT16(4600, level);  // and it gets all the way there
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_a_healthy_supply_holds_nothing);
  RUN_TEST(test_a_low_supply_holds_the_move_until_it_recovers);
  RUN_TEST(test_the_line_is_each_units_own);
  RUN_TEST(test_the_hold_is_bounded);
  RUN_TEST(test_the_cap_survives_the_millis_wrap);
  RUN_TEST(test_nothing_to_judge_by_holds_nothing);
  RUN_TEST(test_a_reset_forgets_the_wait);
  RUN_TEST(test_the_idle_level_takes_a_higher_reading_at_once);
  RUN_TEST(test_a_neighbours_move_does_not_pull_the_level_down);
  RUN_TEST(test_a_rail_that_stays_lower_becomes_the_level);
  return UNITY_END();
}
