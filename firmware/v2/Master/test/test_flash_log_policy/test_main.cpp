// Host-side tests for the flash-log staging/flush/rotation policy (#206).
// The glue (FlashLog.cpp: LittleFS + mutex + netTask tick) is bench-tier;
// everything decidable — how much staged input to accept, when a flush is
// due, when the file rotates — lives here.

#include <unity.h>

#include <cstring>

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

// --- early flush on a warning or an error ----------------------------------------
// A line that reports a failure asks netTask to flush before the 5 s period,
// so a hang or power cut right after it keeps the line. The early flush has
// its own floor so a storm of such lines cannot make netTask hammer flash.

static void test_urgent_flushes_after_the_floor_not_before() {
  TEST_ASSERT_FALSE(flashLogShouldFlush(1, 1024, FLASH_LOG_URGENT_MIN_MS - 1,
                                        5000, true));
  TEST_ASSERT_TRUE(flashLogShouldFlush(1, 1024, FLASH_LOG_URGENT_MIN_MS,
                                       5000, true));
}

static void test_urgent_with_nothing_staged_never_flushes() {
  TEST_ASSERT_FALSE(flashLogShouldFlush(0, 1024, 60000, 5000, true));
}

static void test_urgent_floor_is_below_the_period() {
  TEST_ASSERT_TRUE(FLASH_LOG_URGENT_MIN_MS < FLASH_LOG_FLUSH_INTERVAL_MS);
  TEST_ASSERT_TRUE(FLASH_LOG_URGENT_MIN_MS >= 1000);
}

// A warning on every 10 ms netTask pass for 60 s: at most one flush per floor.
static void test_urgent_storm_is_bounded_by_the_floor() {
  const uint32_t tickMs = 10, spanMs = 60000;
  uint32_t lastFlush = 0, flushes = 0;
  for (uint32_t now = tickMs; now <= spanMs; now += tickMs) {
    if (flashLogShouldFlush(64, FLASH_LOG_STAGE_CAP, now - lastFlush,
                            FLASH_LOG_FLUSH_INTERVAL_MS, true)) {
      flushes++;
      lastFlush = now;
    }
  }
  TEST_ASSERT_EQUAL_UINT32(spanMs / FLASH_LOG_URGENT_MIN_MS, flushes);
}

static bool urgent(const char* s) { return flashLogTextUrgent(s, strlen(s)); }

static void test_failure_lines_are_urgent() {
  TEST_ASSERT_TRUE(urgent("release: the update failed: timeout\n"));
  TEST_ASSERT_TRUE(urgent("Unit 0x12 LOST — no heartbeat, off the bus\n"));
  TEST_ASSERT_TRUE(urgent("crash: reset by Task watchdog — tasks at the time:\n"));
  TEST_ASSERT_TRUE(urgent("FATAL: snapshotMutex allocation failed\n"));
  TEST_ASSERT_TRUE(urgent("Master OTA upload stalled >30 s — aborting\n"));
  TEST_ASSERT_TRUE(urgent("Unit 0x05: jam (stalled move)\n"));
  TEST_ASSERT_TRUE(urgent("liveness: probe Error\n"));
}

static void test_ordinary_lines_are_not_urgent() {
  TEST_ASSERT_FALSE(urgent("netTask up on core 0\n"));
  TEST_ASSERT_FALSE(urgent("link: row1 joined (rev 1a2b3c)\n"));
  TEST_ASSERT_FALSE(urgent("wifi: tx 8.5 dBm (rssi -61)\n"));
  // A refusal answers a caller; it is not a fault of the board.
  TEST_ASSERT_FALSE(urgent("MQTT: restart of the units refused: busy\n"));
  TEST_ASSERT_FALSE(urgent(""));
  TEST_ASSERT_FALSE(flashLogTextUrgent(nullptr, 0));
}

// A keyword counts only as a whole word: a longer word that contains it, or
// starts with it, is not a failure.
static void test_keywords_match_whole_words_only() {
  TEST_ASSERT_FALSE(urgent("Rescue install started\n"));
  TEST_ASSERT_FALSE(urgent("Rescue image installed\n"));
  TEST_ASSERT_FALSE(urgent("reflash: boot auto-install/auto-update skipped\n"));
  TEST_ASSERT_FALSE(urgent("wifi: failover to the second network\n"));
  TEST_ASSERT_FALSE(urgent("traffic jammer off\n"));
  TEST_ASSERT_TRUE(urgent("Unit 0x05: jam (stalled move)\n"));
  TEST_ASSERT_TRUE(urgent("x failed\n"));
}

// A counter names a failure without reporting one: a keyword written as a
// field (`errors=`, `error=`) or right after a number is a count.
static void test_counters_are_not_failures() {
  TEST_ASSERT_FALSE(urgent("Unit 0x05 rebooted (power-on) — brownouts=0 watchdogs=0\n"));
  TEST_ASSERT_FALSE(urgent("stats: errors=0 error=0\n"));
  TEST_ASSERT_FALSE(urgent("link: row1 updated (rev abc, 0 of 3 failed offers)\n"));
  TEST_ASSERT_TRUE(urgent("link: row1 failed (rev abc)\n"));
}

// Generated from tests/flash_log_urgency.txt: every log literal on this
// board that carries a keyword, with the verdict decided for it.
#include "urgency_samples.h"

static void test_every_logged_literal_gets_its_decided_verdict() {
  TEST_ASSERT_TRUE(sizeof(FLASH_LOG_URGENCY_SAMPLES) /
                       sizeof(FLASH_LOG_URGENCY_SAMPLES[0]) > 10);
  for (const auto& sample : FLASH_LOG_URGENCY_SAMPLES) {
    TEST_ASSERT_EQUAL_MESSAGE(sample.urgent, urgent(sample.text), sample.text);
  }
}

// While the flash will not take a flush, urgency stands down: the failure
// line the failed write itself produces must not re-arm a flush a second.
static void test_urgency_stands_down_while_the_last_flush_failed() {
  TEST_ASSERT_FALSE(flashLogShouldFlush(1, 1024, FLASH_LOG_URGENT_MIN_MS,
                                        5000, true, true));
  TEST_ASSERT_TRUE(flashLogShouldFlush(1, 1024, 5000, 5000, true, true));
  TEST_ASSERT_TRUE(flashLogShouldFlush(512, 1024, 0, 5000, true, true));
}

static void test_failing_flash_is_tried_at_the_period_only() {
  const uint32_t tickMs = 10, spanMs = 60000;
  uint32_t lastFlush = 0, flushes = 0;
  for (uint32_t now = tickMs; now <= spanMs; now += tickMs) {
    if (flashLogShouldFlush(64, FLASH_LOG_STAGE_CAP, now - lastFlush,
                            FLASH_LOG_FLUSH_INTERVAL_MS, true, true)) {
      flushes++;
      lastFlush = now;
    }
  }
  TEST_ASSERT_EQUAL_UINT32(spanMs / FLASH_LOG_FLUSH_INTERVAL_MS, flushes);
}

// Chunks are not NUL-terminated: the scan stops at len.
static void test_urgent_scan_stops_at_len() {
  const char buf[] = "ok then failed";
  TEST_ASSERT_FALSE(flashLogTextUrgent(buf, 8));
  TEST_ASSERT_TRUE(flashLogTextUrgent(buf, sizeof(buf) - 1));
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
  RUN_TEST(test_urgent_flushes_after_the_floor_not_before);
  RUN_TEST(test_urgent_with_nothing_staged_never_flushes);
  RUN_TEST(test_urgent_floor_is_below_the_period);
  RUN_TEST(test_urgent_storm_is_bounded_by_the_floor);
  RUN_TEST(test_failure_lines_are_urgent);
  RUN_TEST(test_ordinary_lines_are_not_urgent);
  RUN_TEST(test_urgent_scan_stops_at_len);
  RUN_TEST(test_keywords_match_whole_words_only);
  RUN_TEST(test_counters_are_not_failures);
  RUN_TEST(test_every_logged_literal_gets_its_decided_verdict);
  RUN_TEST(test_urgency_stands_down_while_the_last_flush_failed);
  RUN_TEST(test_failing_flash_is_tried_at_the_period_only);
  return UNITY_END();
}
