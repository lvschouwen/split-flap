// Host-side tests for the flash-log staging/flush/rotation policy (#206).
// The glue (FlashLog.cpp: LittleFS + mutex + netTask tick) is bench-tier;
// everything decidable — how much staged input to accept, when a flush is
// due, when the file rotates — lives here.

#include <unity.h>

#include "../../FlashLogPolicy.h"

void setUp() {}
void tearDown() {}

// --- line stamping into the staging buffer ----------------------------------------
// Every line gets a timestamp prefix at stage time; the stamper tracks
// line starts across arbitrary chunk boundaries and clips at capacity.

static void test_stamper_prefixes_each_line() {
  LogLineStamper st;
  char out[64];
  size_t consumed = 0;
  size_t n = logStamperApply(st, "[T] ", "one\ntwo\n", 8, out, sizeof(out),
                             consumed);
  out[n] = '\0';
  TEST_ASSERT_EQUAL_STRING("[T] one\n[T] two\n", out);
  TEST_ASSERT_EQUAL(8, consumed);
}

static void test_stamper_tracks_line_state_across_chunks() {
  LogLineStamper st;
  char out[64];
  size_t consumed = 0;
  size_t n = logStamperApply(st, "[T] ", "par", 3, out, sizeof(out), consumed);
  n += logStamperApply(st, "[U] ", "tial\nnext", 9, out + n, sizeof(out) - n,
                       consumed);
  out[n] = '\0';
  // Mid-line continuation gets NO stamp; the next line start does (with the
  // stamp current at that moment).
  TEST_ASSERT_EQUAL_STRING("[T] partial\n[U] next", out);
}

static void test_stamper_clips_at_capacity_and_reports_consumed() {
  LogLineStamper st;
  char out[8];  // room for the stamp + 4 payload bytes only
  size_t consumed = 0;
  size_t n = logStamperApply(st, "[T] ", "abcdefgh", 8, out, sizeof(out),
                             consumed);
  TEST_ASSERT_EQUAL(8, n);
  TEST_ASSERT_EQUAL(4, consumed);  // caller counts len - consumed as dropped
  TEST_ASSERT_EQUAL_MEMORY("[T] abcd", out, 8);
}

static void test_stamper_full_output_consumes_nothing() {
  LogLineStamper st;
  char out[2];  // can't even fit the stamp at a line start
  size_t consumed = 0;
  size_t n = logStamperApply(st, "[T] ", "x", 1, out, sizeof(out), consumed);
  TEST_ASSERT_EQUAL(0, n);
  TEST_ASSERT_EQUAL(0, consumed);
}

static void test_stamp_render_clock_and_boot_forms() {
  char stamp[LOG_STAMP_MAX];
  logStampClock(stamp, 9, 5, 7);
  TEST_ASSERT_EQUAL_STRING("[09:05:07] ", stamp);
  logStampBoot(stamp, 42317);  // ms since boot
  TEST_ASSERT_EQUAL_STRING("[+00042.317] ", stamp);
}

// --- flush timing ----------------------------------------------------------------

static void test_flush_on_half_full() {
  TEST_ASSERT_FALSE(flashLogShouldFlush(511, 1024, 0, 5000));
  TEST_ASSERT_TRUE(flashLogShouldFlush(512, 1024, 0, 5000));
}

static void test_flush_on_interval_only_with_content() {
  TEST_ASSERT_TRUE(flashLogShouldFlush(1, 1024, 5000, 5000));
  // Nothing staged: never touch the flash, no matter how long it's been.
  TEST_ASSERT_FALSE(flashLogShouldFlush(0, 1024, 60000, 5000));
}

static void test_no_flush_when_fresh_and_small() {
  TEST_ASSERT_FALSE(flashLogShouldFlush(1, 1024, 4999, 5000));
}

// --- rotation --------------------------------------------------------------------

static void test_rotate_at_file_cap() {
  TEST_ASSERT_FALSE(flashLogShouldRotate(FLASH_LOG_FILE_CAP - 1));
  TEST_ASSERT_TRUE(flashLogShouldRotate(FLASH_LOG_FILE_CAP));
}

// --- day markers -------------------------------------------------------------------
// Days are encoded yyyymmdd; 0 = "unknown" (clock not NTP-synced yet /
// no marker written yet).

static void test_day_marker_never_before_clock_sync() {
  TEST_ASSERT_FALSE(flashLogDayMarkerDue(0, 0));
  TEST_ASSERT_FALSE(flashLogDayMarkerDue(20260711, 0));
}

static void test_day_marker_on_first_synced_flush() {
  TEST_ASSERT_TRUE(flashLogDayMarkerDue(0, 20260711));
}

static void test_day_marker_on_date_change_only() {
  TEST_ASSERT_FALSE(flashLogDayMarkerDue(20260711, 20260711));
  TEST_ASSERT_TRUE(flashLogDayMarkerDue(20260711, 20260712));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_stamper_prefixes_each_line);
  RUN_TEST(test_stamper_tracks_line_state_across_chunks);
  RUN_TEST(test_stamper_clips_at_capacity_and_reports_consumed);
  RUN_TEST(test_stamper_full_output_consumes_nothing);
  RUN_TEST(test_stamp_render_clock_and_boot_forms);
  RUN_TEST(test_flush_on_half_full);
  RUN_TEST(test_flush_on_interval_only_with_content);
  RUN_TEST(test_no_flush_when_fresh_and_small);
  RUN_TEST(test_rotate_at_file_cap);
  RUN_TEST(test_day_marker_never_before_clock_sync);
  RUN_TEST(test_day_marker_on_first_synced_flush);
  RUN_TEST(test_day_marker_on_date_change_only);
  return UNITY_END();
}
