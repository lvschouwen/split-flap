// Host-side tests for the stall timing in UnitStallPolicy.h (#374, #573): how
// long a move may take before the unit reports it as stalled. The
// rotateToLetter() glue that times the move is bench tier.

#include <unity.h>
#include <stdint.h>
#include "../../UnitStallPolicy.h"

void setUp() {}
void tearDown() {}

#define STEPS_PER_REV 2038
#define HOMING_RPM 10
// One step at HOMING_RPM, as the stepper library waits it: 2944 us.
#define HOMING_STEP_US (60000000UL / ((uint32_t)STEPS_PER_REV * HOMING_RPM))

static uint32_t msAtHomingSpeed(uint32_t steps) { return steps * HOMING_STEP_US / 1000UL; }

static void test_expected_time_is_the_steps_at_the_step_period() {
  TEST_ASSERT_EQUAL_UINT32(2944, stallExpectedMoveMs(1000, 10, STEPS_PER_REV));
  TEST_ASSERT_EQUAL_UINT32(0, stallExpectedMoveMs(0, 10, STEPS_PER_REV));
}

static void test_a_speed_below_one_counts_as_one() {
  TEST_ASSERT_EQUAL_UINT32(stallExpectedMoveMs(100, 1, STEPS_PER_REV),
                           stallExpectedMoveMs(100, 0, STEPS_PER_REV));
}

static void test_a_move_by_way_of_home_expects_the_offset_steps_too() {
  // 256 steps to the marker, 69 on to the calibrated zero, no flap steps.
  TEST_ASSERT_EQUAL_UINT32(msAtHomingSpeed(256 + 69),
                           stallExpectedSeekMoveMs(256, 69, 0, HOMING_RPM, 10, STEPS_PER_REV));
}

static void test_a_healthy_short_seek_to_blank_is_not_a_stall() {
  // What the wall's units did (#573): a seek of about 250 steps from one of
  // the last flaps, then the offset, at exactly the commanded step period.
  const int16_t offsets[] = {44, 69, 80, 89};
  for (unsigned i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
    const uint32_t actual = msAtHomingSpeed(240 + offsets[i]);
    const uint32_t expected =
        stallExpectedSeekMoveMs(240, offsets[i], 0, HOMING_RPM, 10, STEPS_PER_REV);
    TEST_ASSERT_FALSE(stallExceeded(actual, expected));
  }
}

static void test_a_negative_offset_takes_its_steps_as_well() {
  TEST_ASSERT_EQUAL_UINT32(stallExpectedSeekMoveMs(256, 69, 0, HOMING_RPM, 10, STEPS_PER_REV),
                           stallExpectedSeekMoveMs(256, -69, 0, HOMING_RPM, 10, STEPS_PER_REV));
}

static void test_the_flap_steps_are_timed_at_the_commanded_speed() {
  const uint32_t seek = stallExpectedSeekMoveMs(256, 69, 0, HOMING_RPM, 15, STEPS_PER_REV);
  TEST_ASSERT_EQUAL_UINT32(seek + stallExpectedMoveMs(453, 15, STEPS_PER_REV),
                           stallExpectedSeekMoveMs(256, 69, 453, HOMING_RPM, 15, STEPS_PER_REV));
}

static void test_a_quarter_over_is_allowed_and_more_is_a_stall() {
  TEST_ASSERT_FALSE(stallExceeded(1000, 1000));
  TEST_ASSERT_FALSE(stallExceeded(1250, 1000));
  TEST_ASSERT_TRUE(stallExceeded(1251, 1000));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_expected_time_is_the_steps_at_the_step_period);
  RUN_TEST(test_a_speed_below_one_counts_as_one);
  RUN_TEST(test_a_move_by_way_of_home_expects_the_offset_steps_too);
  RUN_TEST(test_a_healthy_short_seek_to_blank_is_not_a_stall);
  RUN_TEST(test_a_negative_offset_takes_its_steps_as_well);
  RUN_TEST(test_the_flap_steps_are_timed_at_the_commanded_speed);
  RUN_TEST(test_a_quarter_over_is_allowed_and_more_is_a_stall);
  return UNITY_END();
}
