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

// --- uptime stamps (#503) ---

static void test_every_line_opens_with_its_uptime_stamp() {
  FollowerLogRing r;
  r.appendStamped("a\n", 2, 0);
  r.appendStamped("b", 1, 7);
  r.appendStamped("c\n", 2, 8);  // same line, written in two calls
  String out;
  r.readSince(0, out);
  TEST_ASSERT_EQUAL_STRING("[0] a\n[7] bc\n", out.c_str());
}

static void test_stamp_survives_the_largest_uptime() {
  FollowerLogRing r;
  r.appendStamped("z\n", 2, 0xFFFFFFFFUL);
  String out;
  r.readSince(0, out);
  TEST_ASSERT_EQUAL_STRING("[4294967295] z\n", out.c_str());
}

// --- zero-copy reader (#503/#519) ---

static String viaSink(const FollowerLogRing& r, uint32_t after, uint32_t* next,
                      int* spans) {
  String out;
  int n = 0;
  uint32_t nx = r.readSinceInto(after, [&](const char* p, size_t len) {
    n++;
    for (size_t i = 0; i < len; i++) out += p[i];
  });
  if (next) *next = nx;
  if (spans) *spans = n;
  return out;
}

static void test_sink_reader_matches_the_copying_reader_everywhere() {
  // Fill past a wrap in small steps and compare both readers from every
  // cursor a leader could hold, including stale and future ones.
  FollowerLogRing r;
  const char* chunk = "abcdefg";
  for (int step = 0; step < 9; step++) {
    r.append(chunk, 7);
    for (uint32_t after = 0; after <= r.written + 3; after++) {
      String viaCopy;
      uint32_t nextCopy = r.readSince(after, viaCopy);
      uint32_t nextSink = 0;
      String got = viaSink(r, after, &nextSink, nullptr);
      TEST_ASSERT_EQUAL_STRING(viaCopy.c_str(), got.c_str());
      TEST_ASSERT_EQUAL_UINT32(nextCopy, nextSink);
      TEST_ASSERT_EQUAL_size_t(viaCopy.length(), r.countSince(after));
    }
  }
}

static void test_sink_reader_hands_out_at_most_two_spans_and_none_when_empty() {
  FollowerLogRing r;
  int spans = -1;
  viaSink(r, 0, nullptr, &spans);
  TEST_ASSERT_EQUAL_INT(0, spans);
  r.append("0123456789", 10);  // not wrapped: one span
  viaSink(r, 0, nullptr, &spans);
  TEST_ASSERT_EQUAL_INT(1, spans);
  r.append("abcdefghij", 10);  // 20 bytes into a 16-byte ring: wrapped
  String got = viaSink(r, 0, nullptr, &spans);
  TEST_ASSERT_EQUAL_INT(2, spans);
  TEST_ASSERT_EQUAL_STRING("456789abcdefghij", got.c_str());
  // Caught up: nothing to hand out.
  viaSink(r, r.written, nullptr, &spans);
  TEST_ASSERT_EQUAL_INT(0, spans);
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
  RUN_TEST(test_every_line_opens_with_its_uptime_stamp);
  RUN_TEST(test_stamp_survives_the_largest_uptime);
  RUN_TEST(test_sink_reader_matches_the_copying_reader_everywhere);
  RUN_TEST(test_sink_reader_hands_out_at_most_two_spans_and_none_when_empty);
  return UNITY_END();
}
