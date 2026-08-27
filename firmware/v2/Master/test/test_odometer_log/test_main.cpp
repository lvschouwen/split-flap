// Host-side tests for OdometerLogPolicy.h (#465) — the pure decisions
// behind the odometer historian: when a master-read revolution count earns
// a row on the storage LittleFS, how a reset is recorded as a fact, and
// the row/rotation invariants.

#include <unity.h>

#include <cstring>

#include "../../OdometerLogPolicy.h"

void setUp() {}
void tearDown() {}

// A synced wall-clock epoch, comfortably past ODOLOG_MIN_EPOCH.
static const uint32_t T0 = 1787000000UL;  // 2026-08-something

// --- odologDecide ------------------------------------------------------------

static void test_invalid_reading_never_logs() {
  OdologSlotState s;
  TEST_ASSERT_EQUAL(ODOLOG_SKIP, odologDecide(s, T0, 1234, false));
}

static void test_unsynced_clock_never_logs() {
  // A 1970 (or otherwise pre-sync) stamp would sort the series wrong
  // forever — better no row than a lying row.
  OdologSlotState s;
  TEST_ASSERT_EQUAL(ODOLOG_SKIP, odologDecide(s, 12345, 1234, true));
  TEST_ASSERT_EQUAL(ODOLOG_SKIP,
                    odologDecide(s, ODOLOG_MIN_EPOCH - 1, 1234, true));
}

static void test_first_valid_read_appends_boot_baseline() {
  OdologSlotState s;
  TEST_ASSERT_EQUAL(ODOLOG_APPEND, odologDecide(s, T0, 0, true));
  // Even a zero count is a fact worth a baseline row.
}

static void test_unchanged_value_does_not_relog_after_interval() {
  OdologSlotState s;
  odologApplyAppend(s, T0, 500);
  TEST_ASSERT_EQUAL(ODOLOG_SKIP,
                    odologDecide(s, T0 + 2 * ODOLOG_INTERVAL_S, 500, true));
}

static void test_changed_value_logs_only_after_the_interval() {
  OdologSlotState s;
  odologApplyAppend(s, T0, 500);
  TEST_ASSERT_EQUAL(ODOLOG_SKIP, odologDecide(s, T0 + 60, 501, true));
  TEST_ASSERT_EQUAL(ODOLOG_SKIP,
                    odologDecide(s, T0 + ODOLOG_INTERVAL_S - 1, 999, true));
  TEST_ASSERT_EQUAL(ODOLOG_APPEND,
                    odologDecide(s, T0 + ODOLOG_INTERVAL_S, 999, true));
}

static void test_backwards_jump_is_a_reset_row_immediately() {
  // #417's sweep and #463's ring resize both zeroed every odometer — the
  // discontinuity is recorded the tick it is seen, not a day later.
  OdologSlotState s;
  odologApplyAppend(s, T0, 2737);
  TEST_ASSERT_EQUAL(ODOLOG_APPEND_RESET, odologDecide(s, T0 + 60, 0, true));
  odologApplyAppend(s, T0 + 60, 0);
  // Wear resumes accumulating in the new epoch on the normal cadence.
  TEST_ASSERT_EQUAL(ODOLOG_SKIP, odologDecide(s, T0 + 120, 3, true));
  TEST_ASSERT_EQUAL(ODOLOG_APPEND,
                    odologDecide(s, T0 + 60 + ODOLOG_INTERVAL_S, 3, true));
}

static void test_transient_read_gap_does_not_disturb_state() {
  // A probe rescan or checksum reject drops odometerValid for a tick; the
  // slot's baseline must survive so the gap doesn't fake a fresh boot row.
  OdologSlotState s;
  odologApplyAppend(s, T0, 500);
  TEST_ASSERT_EQUAL(ODOLOG_SKIP, odologDecide(s, T0 + 60, 0, false));
  TEST_ASSERT_EQUAL(ODOLOG_SKIP, odologDecide(s, T0 + 120, 500, true));
}

// --- odologFormatRow ---------------------------------------------------------

static void test_row_format_plain_and_reset() {
  char row[40];
  size_t n = odologFormatRow(row, sizeof(row), T0, 5, 2737, false);
  TEST_ASSERT_TRUE(n > 0);
  TEST_ASSERT_EQUAL_STRING("1787000000,5,2737\n", row);
  n = odologFormatRow(row, sizeof(row), T0, 21, 0, true);
  TEST_ASSERT_TRUE(n > 0);
  TEST_ASSERT_EQUAL_STRING("1787000000,21,0,R\n", row);
}

static void test_row_overflow_returns_zero() {
  char tiny[8];
  TEST_ASSERT_EQUAL_size_t(
      0, odologFormatRow(tiny, sizeof(tiny), T0, 16, 4294967295UL, true));
}

static void test_worst_case_row_fits_the_glue_buffer() {
  // OdometerLog.cpp writes rows into a char[40]; the widest possible row
  // (max epoch, max address, max count, reset flag) must fit it.
  char row[40];
  size_t n = odologFormatRow(row, sizeof(row), 4294967295UL, 255,
                             4294967295UL, true);
  TEST_ASSERT_TRUE(n > 0);
  TEST_ASSERT_EQUAL_size_t(n, strlen(row));
}

// --- rotation ---------------------------------------------------------------

static void test_rotation_triggers_at_the_cap() {
  TEST_ASSERT_FALSE(odologShouldRotate(0));
  TEST_ASSERT_FALSE(odologShouldRotate(ODOLOG_FILE_CAP - 1));
  TEST_ASSERT_TRUE(odologShouldRotate(ODOLOG_FILE_CAP));
}

// ---------------------------------------------------------------------------

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_invalid_reading_never_logs);
  RUN_TEST(test_unsynced_clock_never_logs);
  RUN_TEST(test_first_valid_read_appends_boot_baseline);
  RUN_TEST(test_unchanged_value_does_not_relog_after_interval);
  RUN_TEST(test_changed_value_logs_only_after_the_interval);
  RUN_TEST(test_backwards_jump_is_a_reset_row_immediately);
  RUN_TEST(test_transient_read_gap_does_not_disturb_state);
  RUN_TEST(test_row_format_plain_and_reset);
  RUN_TEST(test_row_overflow_returns_zero);
  RUN_TEST(test_worst_case_row_fits_the_glue_buffer);
  RUN_TEST(test_rotation_triggers_at_the_cap);
  return UNITY_END();
}
