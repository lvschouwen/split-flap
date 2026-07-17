// Host-side tests for the ESP-01 follower log ring (#318 E). The ring is the
// row's only observability window (no serial — GPIO1/3 are the unit bus), so
// the cursor math the leader pulls against is what these tests pin: monotonic
// cursor, delta-since-cursor, wrap eviction, and the stale-cursor rewind that
// keeps a follower reboot from re-dumping the whole ring.

#include <ArduinoFake.h>
#include <unity.h>

// Small ring makes wrap cheap to exercise.
#define FOLLOWER_LOG_SIZE 16
#include "../../FollowerLog.h"

void setUp() {}
void tearDown() {}

static void test_fresh_ring_reads_empty_cursor_zero() {
  FollowerLogRing r;
  String out;
  TEST_ASSERT_EQUAL_UINT32(0, r.readSince(0, out));
  TEST_ASSERT_EQUAL_STRING("", out.c_str());
}

static void test_append_advances_cursor_and_reads_from_zero() {
  FollowerLogRing r;
  r.append("hello\n", 6);
  String out;
  TEST_ASSERT_EQUAL_UINT32(6, r.readSince(0, out));
  TEST_ASSERT_EQUAL_STRING("hello\n", out.c_str());
}

static void test_delta_since_cursor_returns_only_new_bytes() {
  FollowerLogRing r;
  r.append("aaa", 3);
  String first;
  uint32_t cur = r.readSince(0, first);  // cur == 3
  r.append("bbb", 3);
  String delta;
  TEST_ASSERT_EQUAL_UINT32(6, r.readSince(cur, delta));
  TEST_ASSERT_EQUAL_STRING("bbb", delta.c_str());
}

static void test_reading_at_current_cursor_yields_nothing() {
  FollowerLogRing r;
  r.append("xy", 2);
  String out;
  TEST_ASSERT_EQUAL_UINT32(2, r.readSince(2, out));
  TEST_ASSERT_EQUAL_STRING("", out.c_str());
}

static void test_wrap_evicts_oldest_but_cursor_stays_monotonic() {
  FollowerLogRing r;  // size 16
  r.append("0123456789abcdefGH", 18);  // 2 bytes fall off the front
  TEST_ASSERT_EQUAL_UINT32(18, r.written);
  String out;
  uint32_t next = r.readSince(0, out);  // 0 clamps to oldest retained (== 2)
  TEST_ASSERT_EQUAL_UINT32(18, next);
  TEST_ASSERT_EQUAL_STRING("23456789abcdefGH", out.c_str());
}

// A cursor pointing into the evicted region is clamped up to what survives.
static void test_stale_evicted_cursor_clamps_to_oldest_retained() {
  FollowerLogRing r;
  r.append("0123456789abcdefGH", 18);  // retains cursors 2..18
  String out;
  r.readSince(1, out);  // 1 is below oldest (2) -> clamp
  TEST_ASSERT_EQUAL_STRING("23456789abcdefGH", out.c_str());
}

// The reboot case: the leader holds a cursor larger than a freshly-rebooted
// ring's `written`. Rewind to `written`, emit nothing — no re-dump storm.
static void test_cursor_past_written_rewinds_and_emits_nothing() {
  FollowerLogRing r;
  r.append("new", 3);  // written == 3
  String out;
  TEST_ASSERT_EQUAL_UINT32(3, r.readSince(9999, out));
  TEST_ASSERT_EQUAL_STRING("", out.c_str());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_fresh_ring_reads_empty_cursor_zero);
  RUN_TEST(test_append_advances_cursor_and_reads_from_zero);
  RUN_TEST(test_delta_since_cursor_returns_only_new_bytes);
  RUN_TEST(test_reading_at_current_cursor_yields_nothing);
  RUN_TEST(test_wrap_evicts_oldest_but_cursor_stays_monotonic);
  RUN_TEST(test_stale_evicted_cursor_clamps_to_oldest_retained);
  RUN_TEST(test_cursor_past_written_rewinds_and_emits_nothing);
  return UNITY_END();
}
