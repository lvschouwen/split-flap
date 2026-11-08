// Native tests for BootTracePolicy.h: the boot-stage ring that survives any
// reset, so a boot that never came online says where it stopped.
#include <unity.h>

#include "BootTracePolicy.h"

void setUp() {}
void tearDown() {}

static void test_begin_appends_a_setup_entry() {
  BootTrace t;
  bootTraceBegin(t, 15);
  TEST_ASSERT_EQUAL(1, t.count);
  TEST_ASSERT_EQUAL(15, t.e[0].resetReason);
  TEST_ASSERT_EQUAL(BOOT_STAGE_SETUP, t.e[0].stage);
}

static void test_mark_only_moves_forward() {
  BootTrace t;
  bootTraceBegin(t, 1);
  TEST_ASSERT_TRUE(bootTraceMark(t, BOOT_STAGE_JOIN));
  TEST_ASSERT_FALSE(bootTraceMark(t, BOOT_STAGE_UNITS));  // later task, earlier stage
  TEST_ASSERT_FALSE(bootTraceMark(t, BOOT_STAGE_JOIN));   // no rewrite
  TEST_ASSERT_EQUAL(BOOT_STAGE_JOIN, t.e[0].stage);
}

static void test_mark_without_begin_is_ignored() {
  BootTrace t;
  TEST_ASSERT_FALSE(bootTraceMark(t, BOOT_STAGE_ONLINE));
  TEST_ASSERT_EQUAL(0, t.count);
}

static void test_ring_drops_the_oldest() {
  BootTrace t;
  for (int i = 0; i < BOOT_TRACE_RING + 2; i++) bootTraceBegin(t, (uint8_t)i);
  TEST_ASSERT_EQUAL(BOOT_TRACE_RING, t.count);
  TEST_ASSERT_EQUAL(2, t.e[0].resetReason);
  TEST_ASSERT_EQUAL(BOOT_TRACE_RING + 1, t.e[BOOT_TRACE_RING - 1].resetReason);
}

static void test_failed_streak_counts_boots_before_this_one() {
  BootTrace t;
  bootTraceBegin(t, 1);
  bootTraceMark(t, BOOT_STAGE_ONLINE);
  bootTraceBegin(t, 15);  // stopped at setup
  bootTraceBegin(t, 15);
  bootTraceMark(t, BOOT_STAGE_JOIN);
  bootTraceBegin(t, 15);  // this boot
  TEST_ASSERT_EQUAL(2, bootTraceFailedStreak(t));
}

static void test_failed_streak_zero_after_online() {
  BootTrace t;
  bootTraceBegin(t, 1);
  bootTraceMark(t, BOOT_STAGE_ONLINE);
  bootTraceBegin(t, 3);
  TEST_ASSERT_EQUAL(0, bootTraceFailedStreak(t));
}

static void test_blob_round_trip() {
  BootTrace t;
  bootTraceBegin(t, 15);
  bootTraceMark(t, BOOT_STAGE_UNITS);
  bootTraceBegin(t, 3);
  uint8_t blob[BOOT_TRACE_BLOB_LEN];
  bootTraceEncode(t, blob);
  BootTrace back;
  TEST_ASSERT_TRUE(bootTraceDecode(blob, sizeof(blob), back));
  TEST_ASSERT_EQUAL(2, back.count);
  TEST_ASSERT_EQUAL(15, back.e[0].resetReason);
  TEST_ASSERT_EQUAL(BOOT_STAGE_UNITS, back.e[0].stage);
  TEST_ASSERT_EQUAL(3, back.e[1].resetReason);
}

static void test_decode_rejects_garbage() {
  uint8_t blob[BOOT_TRACE_BLOB_LEN] = {0};
  BootTrace t;
  TEST_ASSERT_FALSE(bootTraceDecode(blob, sizeof(blob), t));  // bad magic
  blob[0] = BOOT_TRACE_MAGIC;
  blob[1] = BOOT_TRACE_RING + 1;                             // bad count
  TEST_ASSERT_FALSE(bootTraceDecode(blob, sizeof(blob), t));
  TEST_ASSERT_FALSE(bootTraceDecode(blob, 3, t));            // short
  TEST_ASSERT_EQUAL(0, t.count);  // garbage never leaves partial state
}

// An 18-byte blob written before #628: the history survives, alive unknown.
static void test_v1_blob_decodes_with_alive_unknown() {
  const uint8_t v1[18] = {0xB7, 2, 15, BOOT_STAGE_UNITS, 1, BOOT_STAGE_ONLINE};
  BootTrace t;
  TEST_ASSERT_TRUE(bootTraceDecode(v1, sizeof(v1), t));
  TEST_ASSERT_EQUAL(2, t.count);
  TEST_ASSERT_EQUAL(15, t.e[0].resetReason);
  TEST_ASSERT_EQUAL(BOOT_STAGE_ONLINE, t.e[1].stage);
  TEST_ASSERT_EQUAL_UINT16(BOOT_TRACE_ALIVE_UNKNOWN, t.e[0].aliveMin);
  TEST_ASSERT_EQUAL_UINT16(BOOT_TRACE_ALIVE_UNKNOWN, t.e[1].aliveMin);
  // The next boot appends a known entry after the unknown ones.
  bootTraceBegin(t, 1);
  TEST_ASSERT_EQUAL_UINT16(0, t.e[2].aliveMin);
}

static void test_blob_round_trips_alive() {
  const uint8_t v1[18] = {0xB7, 1, 15, BOOT_STAGE_ONLINE};
  BootTrace t;
  TEST_ASSERT_TRUE(bootTraceDecode(v1, sizeof(v1), t));
  bootTraceBegin(t, 1);
  bootTraceAliveTick(t, 1680ULL * 60000ULL);  // 28 h
  uint8_t blob[BOOT_TRACE_BLOB_LEN];
  TEST_ASSERT_EQUAL(35, sizeof(blob));
  bootTraceEncode(t, blob);
  TEST_ASSERT_EQUAL(BOOT_TRACE_MAGIC, blob[0]);
  TEST_ASSERT_EQUAL(BOOT_TRACE_VERSION, blob[18]);
  BootTrace back;
  TEST_ASSERT_TRUE(bootTraceDecode(blob, sizeof(blob), back));
  TEST_ASSERT_EQUAL(2, back.count);
  TEST_ASSERT_EQUAL_UINT16(BOOT_TRACE_ALIVE_UNKNOWN, back.e[0].aliveMin);
  TEST_ASSERT_EQUAL_UINT16(1680, back.e[1].aliveMin);
  TEST_ASSERT_EQUAL(1, back.e[1].resetReason);
}

static void test_alive_step_saturates() {
  TEST_ASSERT_EQUAL_UINT16(0, bootTraceAliveStep(9ULL * 60000ULL + 59999ULL));
  TEST_ASSERT_EQUAL_UINT16(10, bootTraceAliveStep(10ULL * 60000ULL));
  TEST_ASSERT_EQUAL_UINT16(65530, BOOT_TRACE_ALIVE_MAX);
  TEST_ASSERT_EQUAL_UINT16(BOOT_TRACE_ALIVE_MAX, bootTraceAliveStep(65534ULL * 60000ULL));
  // 100 days: far past uint16 minutes and the 49-day millis() wrap.
  TEST_ASSERT_EQUAL_UINT16(BOOT_TRACE_ALIVE_MAX, bootTraceAliveStep(100ULL * 86400000ULL));
  BootTrace t;
  bootTraceBegin(t, 1);
  TEST_ASSERT_TRUE(bootTraceAliveTick(t, 100ULL * 86400000ULL));
  TEST_ASSERT_FALSE(bootTraceAliveTick(t, 200ULL * 86400000ULL));  // saturated: no writes
  TEST_ASSERT_NOT_EQUAL(BOOT_TRACE_ALIVE_UNKNOWN, t.e[0].aliveMin);
}

static void test_alive_tick_writes_only_on_the_10_min_step() {
  BootTrace t;
  TEST_ASSERT_FALSE(bootTraceAliveTick(t, 3600000ULL));  // no boot yet
  bootTraceBegin(t, 1);
  int writes = 0;
  for (uint64_t ms = 0; ms <= 60ULL * 60000ULL; ms += 1000) {
    if (bootTraceAliveTick(t, ms)) writes++;
  }
  TEST_ASSERT_EQUAL(6, writes);  // 10, 20, ... 60 min: one write each
  TEST_ASSERT_EQUAL_UINT16(60, t.e[0].aliveMin);
  TEST_ASSERT_FALSE(bootTraceAliveTick(t, 69ULL * 60000ULL));
  TEST_ASSERT_TRUE(bootTraceAliveTick(t, 70ULL * 60000ULL));
}

static void test_stage_names() {
  TEST_ASSERT_EQUAL_STRING("setup", bootStageName(BOOT_STAGE_SETUP));
  TEST_ASSERT_EQUAL_STRING("online", bootStageName(BOOT_STAGE_ONLINE));
  TEST_ASSERT_EQUAL_STRING("?", bootStageName(99));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_begin_appends_a_setup_entry);
  RUN_TEST(test_mark_only_moves_forward);
  RUN_TEST(test_mark_without_begin_is_ignored);
  RUN_TEST(test_ring_drops_the_oldest);
  RUN_TEST(test_failed_streak_counts_boots_before_this_one);
  RUN_TEST(test_failed_streak_zero_after_online);
  RUN_TEST(test_blob_round_trip);
  RUN_TEST(test_decode_rejects_garbage);
  RUN_TEST(test_v1_blob_decodes_with_alive_unknown);
  RUN_TEST(test_blob_round_trips_alive);
  RUN_TEST(test_alive_step_saturates);
  RUN_TEST(test_alive_tick_writes_only_on_the_10_min_step);
  RUN_TEST(test_stage_names);
  return UNITY_END();
}
