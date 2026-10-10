// Native tests for WallRowEvents.h: the names of a row board's own events.
#include <unity.h>

#include "../../WallRowEvents.h"

void setUp() {}
void tearDown() {}

static void test_the_codes_have_their_names() {
  TEST_ASSERT_EQUAL_STRING("started", wallRowEventName(1));
  TEST_ASSERT_EQUAL_STRING("self-restart", wallRowEventName(2));
  TEST_ASSERT_EQUAL_STRING("low-memory", wallRowEventName(3));
  TEST_ASSERT_EQUAL_STRING("bus-dead", wallRowEventName(4));
  TEST_ASSERT_EQUAL_STRING("bus-lines", wallRowEventName(5));
}

static void test_a_code_this_build_does_not_know_has_none() {
  TEST_ASSERT_EQUAL_STRING("?", wallRowEventName(0));
  TEST_ASSERT_EQUAL_STRING("?", wallRowEventName(200));
  TEST_ASSERT_EQUAL_STRING("?", wallRowEventName(70000));
}

static void test_a_code_past_one_byte_is_kept_as_none() {
  TEST_ASSERT_EQUAL_UINT8(3, wallRowEventDetail(3));
  TEST_ASSERT_EQUAL_UINT8(255, wallRowEventDetail(255));
  TEST_ASSERT_EQUAL_UINT8(0, wallRowEventDetail(256));
  TEST_ASSERT_EQUAL_UINT8(0, wallRowEventDetail(0x10003));
}

static void test_a_row_may_add_a_burst_and_then_one_every_half_minute() {
  WallRowEventBudget budget;
  for (int i = 0; i < WALL_ROW_EVENT_BURST; i++) TEST_ASSERT_TRUE(budget.take(1000));
  TEST_ASSERT_FALSE(budget.take(1000));
  TEST_ASSERT_FALSE(budget.take(1000 + WALL_ROW_EVENT_REFILL_MS - 1));
  TEST_ASSERT_TRUE(budget.take(1000 + WALL_ROW_EVENT_REFILL_MS));
  TEST_ASSERT_FALSE(budget.take(1000 + WALL_ROW_EVENT_REFILL_MS));
}

static void test_a_quiet_row_gets_its_whole_burst_back_and_no_more() {
  WallRowEventBudget budget;
  for (int i = 0; i < WALL_ROW_EVENT_BURST; i++) budget.take(0);
  const uint32_t later = 100UL * WALL_ROW_EVENT_REFILL_MS;
  for (int i = 0; i < WALL_ROW_EVENT_BURST; i++) TEST_ASSERT_TRUE(budget.take(later));
  TEST_ASSERT_FALSE(budget.take(later));
}

static void test_the_budget_survives_the_uptime_clock_wrapping() {
  WallRowEventBudget budget;
  budget.refilledMs = 0xFFFFF000UL;
  budget.left = 0;
  TEST_ASSERT_FALSE(budget.take(0xFFFFF100UL));
  TEST_ASSERT_TRUE(budget.take(0xFFFFF000UL + WALL_ROW_EVENT_REFILL_MS));  // past the wrap
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_the_codes_have_their_names);
  RUN_TEST(test_a_code_this_build_does_not_know_has_none);
  RUN_TEST(test_a_code_past_one_byte_is_kept_as_none);
  RUN_TEST(test_a_row_may_add_a_burst_and_then_one_every_half_minute);
  RUN_TEST(test_a_quiet_row_gets_its_whole_burst_back_and_no_more);
  RUN_TEST(test_the_budget_survives_the_uptime_clock_wrapping);
  return UNITY_END();
}
