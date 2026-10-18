// The restart of every unit of the wall, one at a time (#624).
#include <ArduinoFake.h>
#include <unity.h>
#include <cstring>
#include "../../RestartUnitsPlan.h"

void setUp() {}
void tearDown() {}

static RestartUnitsPlan planOf(uint8_t ownWidth, uint8_t rowWidth) {
  RestartUnitsPlan plan;
  RestartUnitsBoard boards[2];
  int n = 0;
  if (ownWidth > 0) boards[n++] = {RESTART_UNITS_OWN_ROW, ownWidth};
  if (rowWidth > 0) boards[n++] = {0, rowWidth};
  restartUnitsBegin(plan, 7, boards, n, 1000);
  return plan;
}

static void test_a_wall_without_units_is_done_at_once() {
  RestartUnitsPlan plan = planOf(0, 0);
  TEST_ASSERT_EQUAL(RestartUnitsStep::Done, plan.step);
  char text[64];
  restartUnitsSummary(text, sizeof(text), plan);
  TEST_ASSERT_EQUAL_STRING("no units to restart", text);
}

static void test_the_units_are_taken_board_by_board_one_after_the_other() {
  RestartUnitsPlan plan = planOf(2, 1);
  TEST_ASSERT_EQUAL(RestartUnitsStep::Start, plan.step);
  int row;
  uint8_t address;
  TEST_ASSERT_TRUE(restartUnitsCurrent(plan, row, address));
  TEST_ASSERT_EQUAL(RESTART_UNITS_OWN_ROW, row);
  TEST_ASSERT_EQUAL(1, address);
  restartUnitsStarted(plan, 101, 1100);
  TEST_ASSERT_EQUAL(RestartUnitsStep::Wait, plan.step);
  TEST_ASSERT_EQUAL_UINT32(101, plan.unitOp);
  // Nothing moves while the job runs.
  restartUnitsTick(plan, 5000);
  TEST_ASSERT_EQUAL(RestartUnitsStep::Wait, plan.step);
  // The job ended well: the unit gets its time to boot and find home before
  // the next one starts, so one unit at a time draws its inrush.
  restartUnitsJobEnded(plan, true, 6000);
  TEST_ASSERT_EQUAL(RestartUnitsStep::Settle, plan.step);
  restartUnitsTick(plan, 6000 + RESTART_UNITS_SETTLE_MS - 1);
  TEST_ASSERT_EQUAL(RestartUnitsStep::Settle, plan.step);
  restartUnitsTick(plan, 6000 + RESTART_UNITS_SETTLE_MS);
  TEST_ASSERT_EQUAL(RestartUnitsStep::Start, plan.step);
  TEST_ASSERT_TRUE(restartUnitsCurrent(plan, row, address));
  TEST_ASSERT_EQUAL(RESTART_UNITS_OWN_ROW, row);
  TEST_ASSERT_EQUAL(2, address);
  restartUnitsStarted(plan, 102, 30000);
  restartUnitsJobEnded(plan, true, 31000);
  restartUnitsTick(plan, 31000 + RESTART_UNITS_SETTLE_MS);
  TEST_ASSERT_TRUE(restartUnitsCurrent(plan, row, address));
  TEST_ASSERT_EQUAL(0, row);
  TEST_ASSERT_EQUAL(1, address);
  restartUnitsStarted(plan, 103, 60000);
  restartUnitsJobEnded(plan, true, 61000);
  restartUnitsTick(plan, 61000 + RESTART_UNITS_SETTLE_MS);
  TEST_ASSERT_EQUAL(RestartUnitsStep::Done, plan.step);
  TEST_ASSERT_FALSE(restartUnitsCurrent(plan, row, address));
  TEST_ASSERT_EQUAL(3, plan.done);
  TEST_ASSERT_EQUAL(0, plan.failed);
  char text[64];
  restartUnitsSummary(text, sizeof(text), plan);
  TEST_ASSERT_EQUAL_STRING("3 units restarted", text);
}

static void test_a_unit_that_did_not_take_the_restart_is_counted_and_passed_over() {
  RestartUnitsPlan plan = planOf(2, 0);
  restartUnitsStarted(plan, 101, 1100);
  // A failed job needs no settling: the unit did not reboot.
  restartUnitsJobEnded(plan, false, 1200);
  TEST_ASSERT_EQUAL(RestartUnitsStep::Start, plan.step);
  TEST_ASSERT_EQUAL(1, plan.failed);
  int row;
  uint8_t address;
  TEST_ASSERT_TRUE(restartUnitsCurrent(plan, row, address));
  TEST_ASSERT_EQUAL(2, address);
  // A job the row would not take at all counts the same way.
  restartUnitsStartRefused(plan, 1300);
  TEST_ASSERT_EQUAL(RestartUnitsStep::Done, plan.step);
  TEST_ASSERT_EQUAL(2, plan.failed);
  char text[64];
  restartUnitsSummary(text, sizeof(text), plan);
  TEST_ASSERT_EQUAL_STRING("0 units restarted, 2 failed", text);
}

static void test_a_row_busy_with_another_job_is_waited_for_then_given_up_on() {
  RestartUnitsPlan plan = planOf(1, 0);
  // Still the same unit's turn while the wait lasts.
  TEST_ASSERT_TRUE(restartUnitsStartBusy(plan, 1000));
  TEST_ASSERT_EQUAL(RestartUnitsStep::Start, plan.step);
  TEST_ASSERT_TRUE(restartUnitsStartBusy(plan, 1000 + RESTART_UNITS_WAIT_MAX_MS - 1));
  TEST_ASSERT_FALSE(restartUnitsStartBusy(plan, 1000 + RESTART_UNITS_WAIT_MAX_MS));
  TEST_ASSERT_EQUAL(RestartUnitsStep::Done, plan.step);
  TEST_ASSERT_EQUAL(1, plan.failed);
}

static void test_a_job_that_never_ends_is_given_up_on() {
  RestartUnitsPlan plan = planOf(1, 0);
  restartUnitsStarted(plan, 101, 1000);
  restartUnitsTick(plan, 1000 + RESTART_UNITS_WAIT_MAX_MS - 1);
  TEST_ASSERT_EQUAL(RestartUnitsStep::Wait, plan.step);
  restartUnitsTick(plan, 1000 + RESTART_UNITS_WAIT_MAX_MS);
  TEST_ASSERT_EQUAL(RestartUnitsStep::Done, plan.step);
  TEST_ASSERT_EQUAL(1, plan.failed);
}

static void test_a_firmware_update_stops_the_rest() {
  RestartUnitsPlan plan = planOf(3, 0);
  restartUnitsStarted(plan, 101, 1000);
  restartUnitsJobEnded(plan, true, 2000);
  restartUnitsTick(plan, 2000 + RESTART_UNITS_SETTLE_MS);
  TEST_ASSERT_EQUAL(RestartUnitsStep::Start, plan.step);
  restartUnitsStop(plan);
  TEST_ASSERT_EQUAL(RestartUnitsStep::Done, plan.step);
  TEST_ASSERT_TRUE(plan.stopped);
  char text[64];
  restartUnitsSummary(text, sizeof(text), plan);
  TEST_ASSERT_EQUAL_STRING("stopped for a firmware update after 1 units", text);
}

static void test_the_time_wraps() {
  RestartUnitsPlan plan = planOf(1, 0);
  restartUnitsStarted(plan, 101, 0xFFFFFF00UL);
  restartUnitsJobEnded(plan, true, 0xFFFFFFF0UL);
  restartUnitsTick(plan, (uint32_t)(0xFFFFFFF0UL + RESTART_UNITS_SETTLE_MS));  // past zero
  TEST_ASSERT_EQUAL(RestartUnitsStep::Done, plan.step);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_a_wall_without_units_is_done_at_once);
  RUN_TEST(test_the_units_are_taken_board_by_board_one_after_the_other);
  RUN_TEST(test_a_unit_that_did_not_take_the_restart_is_counted_and_passed_over);
  RUN_TEST(test_a_row_busy_with_another_job_is_waited_for_then_given_up_on);
  RUN_TEST(test_a_job_that_never_ends_is_given_up_on);
  RUN_TEST(test_a_firmware_update_stops_the_rest);
  RUN_TEST(test_the_time_wraps);
  return UNITY_END();
}
