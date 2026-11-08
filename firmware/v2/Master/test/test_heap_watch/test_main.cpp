// Native tests for HeapWatchPolicy.h: internal heap below the floor for
// minutes restarts the master, a transient dip never does.
#include <unity.h>

#include <string.h>

#include "HeapWatchPolicy.h"

void setUp() {}
void tearDown() {}

static const uint32_t kLow = HEAP_WATCH_LOW_BYTES - 1;
static const uint32_t kOk = HEAP_WATCH_LOW_BYTES;

static HeapWatchInput at(uint32_t nowMs, uint32_t freeInternal) {
  HeapWatchInput in;
  in.nowMs = nowMs;
  in.freeInternal = freeInternal;
  return in;
}

static void test_the_floor_sits_below_the_measured_low_water() {
  // Leader, 10.6 h up: minHeap 16648 B, a transient it recovered from.
  TEST_ASSERT_TRUE(HEAP_WATCH_LOW_BYTES < 16648U);
  TEST_ASSERT_TRUE(HEAP_WATCH_RESTART_MS >= 5UL * 60UL * 1000UL);
}

static void test_the_normal_heap_does_nothing() {
  HeapWatchState st;
  for (uint32_t t = 0; t < 2 * HEAP_WATCH_RESTART_MS; t += 1000) {
    HeapWatchVerdict v = heapWatchStep(st, at(t, 49684));
    TEST_ASSERT_FALSE(v.restart || v.logLow || v.logRecovered || v.logWithheld);
  }
  TEST_ASSERT_FALSE(st.low);
}

static void test_logs_after_a_minute_then_restarts_after_the_full_time() {
  HeapWatchState st;
  const uint32_t t0 = 1000;
  TEST_ASSERT_FALSE(heapWatchStep(st, at(t0, kLow)).logLow);
  TEST_ASSERT_FALSE(heapWatchStep(st, at(t0 + HEAP_WATCH_LOG_MS - 1, kLow)).logLow);
  TEST_ASSERT_TRUE(heapWatchStep(st, at(t0 + HEAP_WATCH_LOG_MS, kLow)).logLow);
  TEST_ASSERT_FALSE(heapWatchStep(st, at(t0 + HEAP_WATCH_LOG_MS + 1, kLow)).logLow);  // once
  TEST_ASSERT_FALSE(heapWatchStep(st, at(t0 + HEAP_WATCH_RESTART_MS - 1, kLow)).restart);
  TEST_ASSERT_TRUE(heapWatchStep(st, at(t0 + HEAP_WATCH_RESTART_MS, kLow)).restart);
}

static void test_a_recovery_starts_the_clock_over() {
  HeapWatchState st;
  heapWatchStep(st, at(0, kLow));
  heapWatchStep(st, at(HEAP_WATCH_LOG_MS, kLow));  // logged
  HeapWatchVerdict v = heapWatchStep(st, at(HEAP_WATCH_RESTART_MS - 1000, kOk));
  TEST_ASSERT_TRUE(v.logRecovered);
  TEST_ASSERT_FALSE(st.low);
  heapWatchStep(st, at(HEAP_WATCH_RESTART_MS, kLow));
  TEST_ASSERT_FALSE(heapWatchStep(st, at(HEAP_WATCH_RESTART_MS + 1000, kLow)).restart);
  TEST_ASSERT_FALSE(heapWatchStep(st, at(2 * HEAP_WATCH_RESTART_MS - 1, kLow)).restart);
  TEST_ASSERT_TRUE(heapWatchStep(st, at(2 * HEAP_WATCH_RESTART_MS, kLow)).restart);
}

static void test_a_short_dip_is_not_logged_as_recovered() {
  HeapWatchState st;
  heapWatchStep(st, at(0, kLow));
  TEST_ASSERT_FALSE(heapWatchStep(st, at(5000, kOk)).logRecovered);
}

static void test_a_running_job_withholds_the_restart_until_it_ends() {
  HeapWatchState st;
  heapWatchStep(st, at(0, kLow));
  HeapWatchInput in = at(HEAP_WATCH_RESTART_MS, kLow);
  in.restartBlocked = true;
  HeapWatchVerdict v = heapWatchStep(st, in);
  TEST_ASSERT_FALSE(v.restart);
  TEST_ASSERT_TRUE(v.logWithheld);
  in.nowMs += 1000;
  TEST_ASSERT_FALSE(heapWatchStep(st, in).logWithheld);  // said once
  in.restartBlocked = false;
  TEST_ASSERT_TRUE(heapWatchStep(st, in).restart);
}

static void test_the_rate_limit_withholds_the_restart() {
  HeapWatchState st;
  heapWatchStep(st, at(0, kLow));
  HeapWatchInput in = at(HEAP_WATCH_RESTART_MS, kLow);
  in.restartAllowed = false;
  HeapWatchVerdict v = heapWatchStep(st, in);
  TEST_ASSERT_FALSE(v.restart);
  TEST_ASSERT_TRUE(v.logWithheld);
}

static void test_the_lowest_of_the_episode_is_kept() {
  HeapWatchState st;
  heapWatchStep(st, at(0, 9000));
  heapWatchStep(st, at(1000, 7000));
  heapWatchStep(st, at(2000, 8000));
  TEST_ASSERT_EQUAL_UINT32(7000, st.lowest);
}

static void test_the_clock_survives_a_millis_wrap() {
  HeapWatchState st;
  const uint32_t t0 = 0xFFFFFFFFUL - 1000;
  heapWatchStep(st, at(t0, kLow));
  TEST_ASSERT_FALSE(heapWatchStep(st, at(t0 + 2000, kLow)).restart);
  TEST_ASSERT_TRUE(heapWatchStep(st, at(t0 + HEAP_WATCH_RESTART_MS, kLow)).restart);
}

static void test_garbage_after_power_on_is_a_fresh_record() {
  HeapWatchRecord r;
  memset(&r, 0xA5, sizeof(r));
  TEST_ASSERT_FALSE(heapWatchRecordValid(r));
  TEST_ASSERT_EQUAL_UINT32(0, heapWatchRestarts(r));
  heapWatchRecordBoot(r);
  TEST_ASSERT_TRUE(heapWatchRecordValid(r));
  TEST_ASSERT_EQUAL_UINT32(0, heapWatchRestarts(r));
  TEST_ASSERT_TRUE(heapWatchAllowed(r));
}

static void test_one_restart_per_interval() {
  HeapWatchRecord r;
  memset(&r, 0, sizeof(r));
  heapWatchRecordBoot(r);
  heapWatchRecordTaken(r);
  heapWatchRecordBoot(r);  // the boot after the restart keeps it
  TEST_ASSERT_EQUAL_UINT32(1, heapWatchRestarts(r));
  TEST_ASSERT_FALSE(heapWatchAllowed(r));
  for (uint32_t m = 0; m < HEAP_WATCH_MIN_INTERVAL_MIN - 1; m++) heapWatchRecordMinute(r);
  TEST_ASSERT_FALSE(heapWatchAllowed(r));
  heapWatchRecordMinute(r);
  TEST_ASSERT_TRUE(heapWatchAllowed(r));
  r.restarts = 7;  // a flipped field fails the check
  TEST_ASSERT_FALSE(heapWatchRecordValid(r));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_the_floor_sits_below_the_measured_low_water);
  RUN_TEST(test_the_normal_heap_does_nothing);
  RUN_TEST(test_logs_after_a_minute_then_restarts_after_the_full_time);
  RUN_TEST(test_a_recovery_starts_the_clock_over);
  RUN_TEST(test_a_short_dip_is_not_logged_as_recovered);
  RUN_TEST(test_a_running_job_withholds_the_restart_until_it_ends);
  RUN_TEST(test_the_rate_limit_withholds_the_restart);
  RUN_TEST(test_the_lowest_of_the_episode_is_kept);
  RUN_TEST(test_the_clock_survives_a_millis_wrap);
  RUN_TEST(test_garbage_after_power_on_is_a_fresh_record);
  RUN_TEST(test_one_restart_per_interval);
  return UNITY_END();
}
