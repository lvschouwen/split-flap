// Native tests for link/WallLinkLogDamp.h: one thing that repeats is written
// in full a few times per period, then counted and reported in one line.
#include <unity.h>

#include "WallLinkLogDamp.h"

void setUp() {}
void tearDown() {}

static void test_the_first_of_a_period_are_written_in_full() {
  WlLogDamp d;
  for (int i = 0; i < WL_LOG_DAMP_FULL; i++) {
    TEST_ASSERT_TRUE(wlLogDampWrite(d, 1000 + i));
    TEST_ASSERT_FALSE(wlLogDampHolding(d));
  }
}

static void test_the_rest_of_the_period_is_held_and_counted() {
  WlLogDamp d;
  for (int i = 0; i < WL_LOG_DAMP_FULL; i++) wlLogDampWrite(d, 1000);
  for (int i = 0; i < 7; i++) {
    TEST_ASSERT_FALSE(wlLogDampWrite(d, 2000 + i));
    TEST_ASSERT_TRUE(wlLogDampHolding(d));
  }
  TEST_ASSERT_EQUAL_UINT16(0, wlLogDampEnded(d, 1000 + WL_LOG_DAMP_PERIOD_MS - 1));
  TEST_ASSERT_EQUAL_UINT16(7, wlLogDampEnded(d, 1000 + WL_LOG_DAMP_PERIOD_MS));
}

static void test_a_period_that_ended_is_reported_once_and_the_next_starts_in_full() {
  WlLogDamp d;
  for (int i = 0; i < WL_LOG_DAMP_FULL + 2; i++) wlLogDampWrite(d, 1000);
  const uint32_t after = 1000 + WL_LOG_DAMP_PERIOD_MS;
  TEST_ASSERT_EQUAL_UINT16(2, wlLogDampEnded(d, after));
  TEST_ASSERT_EQUAL_UINT16(0, wlLogDampEnded(d, after + 1));
  TEST_ASSERT_FALSE(wlLogDampHolding(d));
  TEST_ASSERT_TRUE(wlLogDampWrite(d, after + 2));
}

static void test_a_quiet_period_reports_nothing() {
  WlLogDamp d;
  TEST_ASSERT_EQUAL_UINT16(0, wlLogDampEnded(d, 5000));
  wlLogDampWrite(d, 5000);
  TEST_ASSERT_EQUAL_UINT16(0, wlLogDampEnded(d, 5000 + WL_LOG_DAMP_PERIOD_MS));
}

static void test_a_period_spans_the_millisecond_counter_wrapping() {
  WlLogDamp d;
  const uint32_t start = 0xFFFFFF00UL;
  for (int i = 0; i < WL_LOG_DAMP_FULL + 1; i++) wlLogDampWrite(d, start);
  TEST_ASSERT_EQUAL_UINT16(0, wlLogDampEnded(d, start + 1000));  // wrapped, still inside
  TEST_ASSERT_EQUAL_UINT16(1, wlLogDampEnded(d, start + WL_LOG_DAMP_PERIOD_MS));
}

static void test_the_count_stops_at_its_ceiling() {
  WlLogDamp d;
  for (uint32_t i = 0; i < 70000UL; i++) wlLogDampWrite(d, 1000);
  TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, wlLogDampEnded(d, 1000 + WL_LOG_DAMP_PERIOD_MS));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_the_first_of_a_period_are_written_in_full);
  RUN_TEST(test_the_rest_of_the_period_is_held_and_counted);
  RUN_TEST(test_a_period_that_ended_is_reported_once_and_the_next_starts_in_full);
  RUN_TEST(test_a_quiet_period_reports_nothing);
  RUN_TEST(test_a_period_spans_the_millisecond_counter_wrapping);
  RUN_TEST(test_the_count_stops_at_its_ceiling);
  return UNITY_END();
}
