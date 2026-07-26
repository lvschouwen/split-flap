// Host-side unit tests for the pure I2C heartbeat / miss-detection logic in
// HeartbeatPolicy.h (#310): the consecutive-miss counter, the stale threshold
// and the round-robin slot advance. The displayTask transport is bench tier.

#include <unity.h>
#include <stdint.h>
#include "HeartbeatPolicy.h"

void setUp() {}
void tearDown() {}

// --- consecutive-miss counter ----------------------------------------------

static void test_good_read_resets_counter() {
  TEST_ASSERT_EQUAL_UINT8(0, heartbeatMissCount(5, true));
  TEST_ASSERT_EQUAL_UINT8(0, heartbeatMissCount(0, true));
}

static void test_miss_increments() {
  TEST_ASSERT_EQUAL_UINT8(1, heartbeatMissCount(0, false));
  TEST_ASSERT_EQUAL_UINT8(4, heartbeatMissCount(3, false));
}

static void test_miss_counter_saturates() {
  TEST_ASSERT_EQUAL_UINT8(255, heartbeatMissCount(255, false));
  TEST_ASSERT_EQUAL_UINT8(255, heartbeatMissCount(254, false));
}

// --- stale threshold -------------------------------------------------------

static void test_stale_at_or_past_threshold() {
  TEST_ASSERT_FALSE(heartbeatIsStale(2, 3));
  TEST_ASSERT_TRUE(heartbeatIsStale(3, 3));
  TEST_ASSERT_TRUE(heartbeatIsStale(9, 3));
}

static void test_default_threshold_is_three() {
  TEST_ASSERT_EQUAL_UINT8(3, HEARTBEAT_MISS_THRESHOLD);
  TEST_ASSERT_FALSE(heartbeatIsStale(2, HEARTBEAT_MISS_THRESHOLD));
  TEST_ASSERT_TRUE(heartbeatIsStale(3, HEARTBEAT_MISS_THRESHOLD));
}

// --- round-robin slot advance ----------------------------------------------

static void test_next_slot_wraps() {
  TEST_ASSERT_EQUAL_INT(1, heartbeatNextSlot(0, 3));
  TEST_ASSERT_EQUAL_INT(2, heartbeatNextSlot(1, 3));
  TEST_ASSERT_EQUAL_INT(0, heartbeatNextSlot(2, 3));  // wrap
}

static void test_next_slot_zero_width_parks() {
  TEST_ASSERT_EQUAL_INT(0, heartbeatNextSlot(0, 0));
  TEST_ASSERT_EQUAL_INT(0, heartbeatNextSlot(5, -1));
}

static void test_next_slot_single_unit_stays() {
  TEST_ASSERT_EQUAL_INT(0, heartbeatNextSlot(0, 1));
}

// --- heartbeatApply: the per-unit freshness fold ---------------------------

static void test_apply_good_read_resets_and_stamps() {
  UnitFacts u;
  u.state = 1;
  u.misses = 2;
  heartbeatApply(u, true, 5000, 3);
  TEST_ASSERT_EQUAL_UINT8(0, u.misses);
  TEST_ASSERT_FALSE(u.stale);
  TEST_ASSERT_EQUAL_UINT32(5000, u.lastSeenMs);
}

static void test_apply_miss_streak_latches_stale() {
  UnitFacts u;
  u.state = 1;
  u.lastSeenMs = 100;
  heartbeatApply(u, false, 5000, 3);
  TEST_ASSERT_EQUAL_UINT8(1, u.misses);
  TEST_ASSERT_FALSE(u.stale);
  heartbeatApply(u, false, 6000, 3);
  heartbeatApply(u, false, 7000, 3);
  TEST_ASSERT_EQUAL_UINT8(3, u.misses);
  TEST_ASSERT_TRUE(u.stale);
  // A miss never re-stamps lastSeenMs — that's what makes "age" grow.
  TEST_ASSERT_EQUAL_UINT32(100, u.lastSeenMs);
}

static void test_apply_gap_slot_resets() {
  UnitFacts u;
  u.state = 0;  // silent gap, not a tracked unit
  u.misses = 9;
  u.stale = true;
  heartbeatApply(u, false, 5000, 3);
  TEST_ASSERT_EQUAL_UINT8(0, u.misses);
  TEST_ASSERT_FALSE(u.stale);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_good_read_resets_counter);
  RUN_TEST(test_miss_increments);
  RUN_TEST(test_miss_counter_saturates);
  RUN_TEST(test_stale_at_or_past_threshold);
  RUN_TEST(test_default_threshold_is_three);
  RUN_TEST(test_next_slot_wraps);
  RUN_TEST(test_next_slot_zero_width_parks);
  RUN_TEST(test_next_slot_single_unit_stays);
  RUN_TEST(test_apply_good_read_resets_and_stamps);
  RUN_TEST(test_apply_miss_streak_latches_stale);
  RUN_TEST(test_apply_gap_slot_resets);
  return UNITY_END();
}
