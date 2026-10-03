// Host-side tests for the failed-home retry policy in UnitHomePolicy.h (#502):
// when a seek is allowed again after one that never found the hall marker.
// The calibrate()/loop() glue that asks the question is bench tier.

#include <unity.h>
#include <stdint.h>
#include "../../UnitHomePolicy.h"

void setUp() {}
void tearDown() {}

static HomeBackoff fresh() {
  HomeBackoff b = {0, 0};
  return b;
}

static void test_a_unit_that_never_failed_may_always_home() {
  HomeBackoff b = fresh();
  TEST_ASSERT_TRUE(homeAttemptAllowed(b, 0));
  TEST_ASSERT_TRUE(homeAttemptAllowed(b, 123456789UL));
}

static void test_one_failure_holds_the_next_attempt_for_the_base_wait() {
  HomeBackoff b = fresh();
  homeNoteResult(b, false, 100000UL);
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
  homeNoteResult(b, false, 1000UL);
  homeNoteResult(b, false, 50000UL);
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
  for (int i = 0; i < 400; i++) homeNoteResult(b, false, 5000UL);
  TEST_ASSERT_EQUAL_UINT8(255, b.consecutiveFails);
  TEST_ASSERT_FALSE(homeAttemptAllowed(b, 5000UL + 959999UL));
  TEST_ASSERT_TRUE(homeAttemptAllowed(b, 5000UL + 960000UL));
}

static void test_a_found_marker_clears_the_wait_at_once() {
  HomeBackoff b = fresh();
  homeNoteResult(b, false, 1000UL);
  homeNoteResult(b, false, 2000UL);
  homeNoteResult(b, true, 3000UL);
  TEST_ASSERT_EQUAL_UINT8(0, b.consecutiveFails);
  TEST_ASSERT_TRUE(homeAttemptAllowed(b, 3000UL));
  homeNoteResult(b, false, 4000UL);  // the doubling starts over
  TEST_ASSERT_TRUE(homeAttemptAllowed(b, 4000UL + HOME_RETRY_BASE_MS));
}

static void test_the_wait_is_measured_across_the_millis_wrap() {
  HomeBackoff b = fresh();
  const uint32_t nearWrap = 0xFFFFFFFFUL - 9999UL;  // 10 s before the wrap
  homeNoteResult(b, false, nearWrap);
  TEST_ASSERT_FALSE(homeAttemptAllowed(b, 0xFFFFFFFFUL));
  TEST_ASSERT_FALSE(homeAttemptAllowed(b, 19999UL));   // 29.999 s later
  TEST_ASSERT_TRUE(homeAttemptAllowed(b, 20000UL));    // 30 s later
}

// --- the retry the unit owes itself, and the explicit command ---------------

static void test_a_failed_home_owes_a_retry_until_one_succeeds() {
  HomeBackoff b = fresh();
  TEST_ASSERT_FALSE(homeRetryOwed(b));
  homeNoteResult(b, false, 1000UL);
  TEST_ASSERT_TRUE(homeRetryOwed(b));
  homeNoteResult(b, false, 90000UL);
  TEST_ASSERT_TRUE(homeRetryOwed(b));
  homeNoteResult(b, true, 200000UL);
  TEST_ASSERT_FALSE(homeRetryOwed(b));
}

// After a repair the operator's HOME must not wait out the minutes the
// automatic retry has backed off to.
static void test_home_command_waits_only_the_base_gap() {
  HomeBackoff b = fresh();
  TEST_ASSERT_TRUE(homeCommandAllowed(b, 0));
  for (int i = 0; i < 6; i++) homeNoteResult(b, false, 500000UL);
  TEST_ASSERT_FALSE(homeAttemptAllowed(b, 500000UL + HOME_RETRY_BASE_MS));
  TEST_ASSERT_FALSE(homeCommandAllowed(b, 500000UL + HOME_RETRY_BASE_MS - 1));
  TEST_ASSERT_TRUE(homeCommandAllowed(b, 500000UL + HOME_RETRY_BASE_MS));
}

// It still waits that base gap: back-to-back HOME commands against a dead
// sensor are exactly what keeps a motor running.
static void test_home_command_is_refused_right_after_a_failure() {
  HomeBackoff b = fresh();
  homeNoteResult(b, false, 7000UL);
  TEST_ASSERT_FALSE(homeCommandAllowed(b, 7000UL));
  TEST_ASSERT_FALSE(homeCommandAllowed(b, 7000UL + 1000UL));
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
  return UNITY_END();
}
