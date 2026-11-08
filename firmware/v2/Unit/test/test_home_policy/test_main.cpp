// Host-side tests for the failed-home retry policy in UnitHomePolicy.h (#502):
// when a seek is allowed again after one that never found the hall marker,
// and when the unit stops seeking by itself altogether (#667).
// The calibrate()/loop() glue that asks the question is bench tier.

#include <unity.h>
#include <stdint.h>
#include "../../UnitHomePolicy.h"

void setUp() {}
void tearDown() {}

static HomeBackoff fresh() {
  HomeBackoff b = {0, 0, 0};
  return b;
}

static void test_a_unit_that_never_failed_may_always_home() {
  HomeBackoff b = fresh();
  TEST_ASSERT_TRUE(homeAttemptAllowed(b, 0));
  TEST_ASSERT_TRUE(homeAttemptAllowed(b, 123456789UL));
}

static void test_one_failure_holds_the_next_attempt_for_the_base_wait() {
  HomeBackoff b = fresh();
  homeNoteResult(b, false, true, 100000UL);
  TEST_ASSERT_FALSE(homeAttemptAllowed(b, 100000UL));
  TEST_ASSERT_FALSE(homeAttemptAllowed(b, 100000UL + HOME_RETRY_BASE_MS - 1));
  TEST_ASSERT_TRUE(homeAttemptAllowed(b, 100000UL + HOME_RETRY_BASE_MS));
}

static void test_each_consecutive_failure_doubles_the_wait() {
  TEST_ASSERT_EQUAL_UINT32(0, homeRetryWaitMs(0));
  TEST_ASSERT_EQUAL_UINT32(30000UL, homeRetryWaitMs(1));
  TEST_ASSERT_EQUAL_UINT32(60000UL, homeRetryWaitMs(2));
  TEST_ASSERT_EQUAL_UINT32(120000UL, homeRetryWaitMs(3));
  HomeBackoff b = fresh();
  homeNoteResult(b, false, true, 1000UL);
  homeNoteResult(b, false, true, 50000UL);
  TEST_ASSERT_FALSE(homeAttemptAllowed(b, 50000UL + 59999UL));
  TEST_ASSERT_TRUE(homeAttemptAllowed(b, 50000UL + 60000UL));
}

// The ceiling is what bounds the motor's duty on a unit that keeps being
// asked, so it is pinned as a number, and so is the fact that it IS a ceiling.
static void test_the_wait_stops_growing_at_sixteen_minutes() {
  const uint32_t cap = (uint32_t)HOME_RETRY_BASE_MS << HOME_RETRY_MAX_SHIFT;
  TEST_ASSERT_EQUAL_UINT32(960000UL, cap);
  TEST_ASSERT_EQUAL_UINT32(cap, homeRetryWaitMs(HOME_RETRY_MAX_SHIFT + 1));
  TEST_ASSERT_EQUAL_UINT32(cap, homeRetryWaitMs(HOME_RETRY_MAX_SHIFT + 2));
  TEST_ASSERT_EQUAL_UINT32(cap, homeRetryWaitMs(255));
}

static void test_the_failure_count_saturates() {
  HomeBackoff b = fresh();
  for (int i = 0; i < 400; i++) homeNoteResult(b, false, true, 5000UL);
  TEST_ASSERT_EQUAL_UINT8(255, b.consecutiveFails);
  TEST_ASSERT_FALSE(homeAttemptAllowed(b, 5000UL + 959999UL));
  TEST_ASSERT_TRUE(homeAttemptAllowed(b, 5000UL + 960000UL));
}

static void test_a_found_marker_clears_the_wait_at_once() {
  HomeBackoff b = fresh();
  homeNoteResult(b, false, true, 1000UL);
  homeNoteResult(b, false, true, 2000UL);
  homeNoteResult(b, true, true, 3000UL);
  TEST_ASSERT_EQUAL_UINT8(0, b.consecutiveFails);
  TEST_ASSERT_TRUE(homeAttemptAllowed(b, 3000UL));
  homeNoteResult(b, false, true, 4000UL);  // the doubling starts over
  TEST_ASSERT_TRUE(homeAttemptAllowed(b, 4000UL + HOME_RETRY_BASE_MS));
}

static void test_the_wait_is_measured_across_the_millis_wrap() {
  HomeBackoff b = fresh();
  const uint32_t nearWrap = 0xFFFFFFFFUL - 9999UL;  // 10 s before the wrap
  homeNoteResult(b, false, true, nearWrap);
  TEST_ASSERT_FALSE(homeAttemptAllowed(b, 0xFFFFFFFFUL));
  TEST_ASSERT_FALSE(homeAttemptAllowed(b, 19999UL));   // 29.999 s later
  TEST_ASSERT_TRUE(homeAttemptAllowed(b, 20000UL));    // 30 s later
}

// --- the retry the unit owes itself, and the explicit command ---------------

static void test_a_failed_home_owes_a_retry_until_one_succeeds() {
  HomeBackoff b = fresh();
  TEST_ASSERT_FALSE(homeRetryOwed(b));
  homeNoteResult(b, false, true, 1000UL);
  TEST_ASSERT_TRUE(homeRetryOwed(b));
  homeNoteResult(b, false, true, 90000UL);
  TEST_ASSERT_TRUE(homeRetryOwed(b));
  homeNoteResult(b, true, true, 200000UL);
  TEST_ASSERT_FALSE(homeRetryOwed(b));
}

// After a repair the operator's HOME must not wait out the minutes the
// automatic retry has backed off to.
static void test_home_command_waits_only_the_base_gap() {
  HomeBackoff b = fresh();
  TEST_ASSERT_TRUE(homeCommandAllowed(b, 0));
  for (int i = 0; i < 6; i++) homeNoteResult(b, false, true, 500000UL);
  TEST_ASSERT_FALSE(homeAttemptAllowed(b, 500000UL + HOME_RETRY_BASE_MS));
  TEST_ASSERT_FALSE(homeCommandAllowed(b, 500000UL + HOME_RETRY_BASE_MS - 1));
  TEST_ASSERT_TRUE(homeCommandAllowed(b, 500000UL + HOME_RETRY_BASE_MS));
}

// It still waits that base gap: back-to-back HOME commands against a dead
// sensor are exactly what keeps a motor running.
static void test_home_command_is_refused_right_after_a_failure() {
  HomeBackoff b = fresh();
  homeNoteResult(b, false, true, 7000UL);
  TEST_ASSERT_FALSE(homeCommandAllowed(b, 7000UL));
  TEST_ASSERT_FALSE(homeCommandAllowed(b, 7000UL + 1000UL));
}

// --- standing down after seeks that never saw the hall (#667) ---------------

static void failHallNever(HomeBackoff& b, uint32_t nowMs) {
  homeNoteResult(b, false, false, nowMs);
}

static void test_three_hall_never_failures_stand_the_unit_down() {
  HomeBackoff b = fresh();
  TEST_ASSERT_EQUAL_INT(3, HOME_STANDDOWN_FAILS);
  failHallNever(b, 1000UL);
  failHallNever(b, 2000UL);
  TEST_ASSERT_FALSE(homeStoodDown(b));
  TEST_ASSERT_TRUE(homeRetryOwed(b));
  failHallNever(b, 3000UL);
  TEST_ASSERT_TRUE(homeStoodDown(b));
  // No automatic retry, however long it waits.
  TEST_ASSERT_FALSE(homeRetryOwed(b));
  TEST_ASSERT_FALSE(homeAttemptAllowed(b, 3000UL + 960000UL));
  TEST_ASSERT_FALSE(homeAttemptAllowed(b, 3000UL + 0x7FFFFFFFUL));
}

// rotateToLetter() asks homeAttemptAllowed() before it seeks for a letter.
static void test_a_letter_while_stood_down_does_not_seek() {
  HomeBackoff b = fresh();
  for (int i = 0; i < HOME_STANDDOWN_FAILS; i++) failHallNever(b, 10000UL);
  for (uint32_t t = 10000UL; t < 10000UL + 4000000UL; t += 100000UL) {
    TEST_ASSERT_FALSE(homeAttemptAllowed(b, t));
  }
}

// HOME gets one seek after the base gap; failing the same way stands the unit
// down again at once, not after three more.
static void test_home_command_gets_exactly_one_try_while_stood_down() {
  HomeBackoff b = fresh();
  for (int i = 0; i < HOME_STANDDOWN_FAILS; i++) failHallNever(b, 50000UL);
  TEST_ASSERT_FALSE(homeCommandAllowed(b, 50000UL + HOME_RETRY_BASE_MS - 1));
  TEST_ASSERT_TRUE(homeCommandAllowed(b, 50000UL + HOME_RETRY_BASE_MS));
  const uint32_t after = 50000UL + HOME_RETRY_BASE_MS + 18000UL;
  failHallNever(b, after);  // the commanded seek
  TEST_ASSERT_TRUE(homeStoodDown(b));
  TEST_ASSERT_FALSE(homeRetryOwed(b));
  TEST_ASSERT_FALSE(homeAttemptAllowed(b, after + 960000UL));
  TEST_ASSERT_FALSE(homeCommandAllowed(b, after + HOME_RETRY_BASE_MS - 1));
}

static void test_a_found_marker_ends_the_stand_down() {
  HomeBackoff b = fresh();
  for (int i = 0; i < HOME_STANDDOWN_FAILS; i++) failHallNever(b, 1000UL);
  homeNoteResult(b, true, true, 40000UL);
  TEST_ASSERT_FALSE(homeStoodDown(b));
  TEST_ASSERT_TRUE(homeAttemptAllowed(b, 40000UL));
  // The count starts over: one more hall-never failure is only a backoff.
  failHallNever(b, 50000UL);
  TEST_ASSERT_FALSE(homeStoodDown(b));
  TEST_ASSERT_TRUE(homeRetryOwed(b));
}

// A seek that saw the hall is a sensor or window problem, not a stall: it
// keeps the doubling backoff and restarts the hall-never count.
static void test_a_hall_seen_failure_resets_the_hall_never_count() {
  HomeBackoff b = fresh();
  failHallNever(b, 1000UL);
  failHallNever(b, 2000UL);
  homeNoteResult(b, false, true, 3000UL);
  failHallNever(b, 4000UL);
  failHallNever(b, 5000UL);
  TEST_ASSERT_FALSE(homeStoodDown(b));
  TEST_ASSERT_TRUE(homeRetryOwed(b));
  TEST_ASSERT_EQUAL_UINT8(5, b.consecutiveFails);  // the backoff still counts all
  TEST_ASSERT_TRUE(homeAttemptAllowed(b, 5000UL + homeRetryWaitMs(5)));
  failHallNever(b, 6000UL);
  TEST_ASSERT_TRUE(homeStoodDown(b));
}

// The commanded seek of a stood-down unit that does see the hall goes back to
// the automatic retries.
static void test_a_hall_seen_failure_after_home_resumes_the_backoff() {
  HomeBackoff b = fresh();
  for (int i = 0; i < HOME_STANDDOWN_FAILS; i++) failHallNever(b, 1000UL);
  homeNoteResult(b, false, true, 60000UL);
  TEST_ASSERT_FALSE(homeStoodDown(b));
  TEST_ASSERT_TRUE(homeRetryOwed(b));
  TEST_ASSERT_TRUE(homeAttemptAllowed(b, 60000UL + homeRetryWaitMs(4)));
}

static void test_the_hall_never_count_saturates_stood_down() {
  HomeBackoff b = fresh();
  for (int i = 0; i < 400; i++) failHallNever(b, 5000UL);
  TEST_ASSERT_EQUAL_UINT8(255, b.hallNeverFails);
  TEST_ASSERT_TRUE(homeStoodDown(b));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_a_unit_that_never_failed_may_always_home);
  RUN_TEST(test_one_failure_holds_the_next_attempt_for_the_base_wait);
  RUN_TEST(test_each_consecutive_failure_doubles_the_wait);
  RUN_TEST(test_the_wait_stops_growing_at_sixteen_minutes);
  RUN_TEST(test_the_failure_count_saturates);
  RUN_TEST(test_a_found_marker_clears_the_wait_at_once);
  RUN_TEST(test_the_wait_is_measured_across_the_millis_wrap);
  RUN_TEST(test_a_failed_home_owes_a_retry_until_one_succeeds);
  RUN_TEST(test_home_command_waits_only_the_base_gap);
  RUN_TEST(test_home_command_is_refused_right_after_a_failure);
  RUN_TEST(test_three_hall_never_failures_stand_the_unit_down);
  RUN_TEST(test_a_letter_while_stood_down_does_not_seek);
  RUN_TEST(test_home_command_gets_exactly_one_try_while_stood_down);
  RUN_TEST(test_a_found_marker_ends_the_stand_down);
  RUN_TEST(test_a_hall_seen_failure_resets_the_hall_never_count);
  RUN_TEST(test_a_hall_seen_failure_after_home_resumes_the_backoff);
  RUN_TEST(test_the_hall_never_count_saturates_stood_down);
  return UNITY_END();
}
