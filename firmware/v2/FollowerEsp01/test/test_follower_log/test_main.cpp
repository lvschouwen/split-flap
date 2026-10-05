// Host-side tests for the ESP-01 row's log ring (#318 E). The ring is the
// row's only observability window (no serial — GPIO1/3 are the unit bus), so
// what these tests pin is the cursor math its reader relies on: monotonic
// cursor, whole lines only, wrap eviction, and a stale cursor.

#include <ArduinoFake.h>
#include <unity.h>

#include <string.h>

// Small ring makes wrap cheap to exercise.
#define FOLLOWER_LOG_SIZE 16
#include "../../FollowerLog.h"

void setUp() {}
void tearDown() {}

// Every whole line from `cursor` on, each closed with '\n' again.
static String drain(const FollowerLogRing& r, uint32_t& cursor, size_t cap = 32) {
  String all;
  char out[32];
  size_t len = 0;
  while (r.nextLine(cursor, out, cap, len)) {
    for (size_t i = 0; i < len; i++) all += out[i];
    all += '\n';
  }
  return all;
}

static void test_fresh_ring_has_no_line() {
  FollowerLogRing r;
  uint32_t cursor = 0;
  TEST_ASSERT_EQUAL_STRING("", drain(r, cursor).c_str());
  TEST_ASSERT_EQUAL_UINT32(0, cursor);
}

static void test_lines_come_whole_without_the_newline() {
  FollowerLogRing r;
  r.append("one\ntwo\n", 8);
  uint32_t cursor = 0;
  char out[32];
  size_t len = 99;
  TEST_ASSERT_TRUE(r.nextLine(cursor, out, sizeof(out), len));
  TEST_ASSERT_EQUAL_size_t(3, len);
  TEST_ASSERT_EQUAL_MEMORY("one", out, 3);
  TEST_ASSERT_EQUAL_UINT32(4, cursor);
  TEST_ASSERT_TRUE(r.nextLine(cursor, out, sizeof(out), len));
  TEST_ASSERT_EQUAL_MEMORY("two", out, 3);
  TEST_ASSERT_EQUAL_UINT32(8, cursor);
  TEST_ASSERT_FALSE(r.nextLine(cursor, out, sizeof(out), len));
  TEST_ASSERT_EQUAL_UINT32(8, cursor);
}

static void test_only_new_lines_follow_a_cursor() {
  FollowerLogRing r;
  r.append("aaa\n", 4);
  uint32_t cursor = 0;
  TEST_ASSERT_EQUAL_STRING("aaa\n", drain(r, cursor).c_str());
  r.append("bbb\n", 4);
  TEST_ASSERT_EQUAL_STRING("bbb\n", drain(r, cursor).c_str());
  TEST_ASSERT_EQUAL_UINT32(8, cursor);
}

static void test_a_line_still_being_written_waits() {
  FollowerLogRing r;
  r.append("half", 4);
  uint32_t cursor = 0;
  char out[32];
  size_t len = 7;
  TEST_ASSERT_FALSE(r.nextLine(cursor, out, sizeof(out), len));
  TEST_ASSERT_EQUAL_UINT32(0, cursor);
  TEST_ASSERT_EQUAL_size_t(7, len);
  r.append(" done\n", 6);
  TEST_ASSERT_TRUE(r.nextLine(cursor, out, sizeof(out), len));
  TEST_ASSERT_EQUAL_size_t(9, len);
  TEST_ASSERT_EQUAL_MEMORY("half done", out, 9);
}

static void test_an_empty_line_is_sent_as_empty() {
  FollowerLogRing r;
  r.append("\nx\n", 3);
  uint32_t cursor = 0;
  char out[8];
  size_t len = 5;
  TEST_ASSERT_TRUE(r.nextLine(cursor, out, sizeof(out), len));
  TEST_ASSERT_EQUAL_size_t(0, len);
  TEST_ASSERT_EQUAL_UINT32(1, cursor);
}

static void test_a_line_longer_than_the_message_comes_in_pieces() {
  FollowerLogRing r;
  r.append("abcdefgh\n", 9);
  uint32_t cursor = 0;
  char out[4];
  size_t len = 0;
  TEST_ASSERT_TRUE(r.nextLine(cursor, out, sizeof(out), len));
  TEST_ASSERT_EQUAL_size_t(4, len);
  TEST_ASSERT_EQUAL_MEMORY("abcd", out, 4);
  TEST_ASSERT_TRUE(r.nextLine(cursor, out, sizeof(out), len));
  TEST_ASSERT_EQUAL_MEMORY("efgh", out, 4);
  // The last piece takes the newline with it: no empty piece follows.
  TEST_ASSERT_EQUAL_UINT32(9, cursor);
  TEST_ASSERT_FALSE(r.nextLine(cursor, out, sizeof(out), len));
}

static void test_wrap_evicts_oldest_but_cursor_stays_monotonic() {
  FollowerLogRing r;  // size 16
  r.append("0123456789abcdeG\nH\n", 19);  // 3 bytes fall off the front
  TEST_ASSERT_EQUAL_UINT32(19, r.written);
  TEST_ASSERT_EQUAL_UINT32(3, r.oldestCursor());
  uint32_t cursor = 0;  // clamps to the oldest byte still held
  TEST_ASSERT_EQUAL_STRING("3456789abcdeG\nH\n", drain(r, cursor).c_str());
  TEST_ASSERT_EQUAL_UINT32(19, cursor);
}

// A cursor pointing into the evicted region is clamped up to what survives.
static void test_stale_evicted_cursor_clamps_to_oldest_retained() {
  FollowerLogRing r;
  r.append("0123456789abcdeG\nH\n", 19);
  uint32_t cursor = 1;
  TEST_ASSERT_EQUAL_STRING("3456789abcdeG\nH\n", drain(r, cursor).c_str());
}

// A cursor from before a restart is past a fresh ring's `written`: nothing
// old is sent again.
static void test_cursor_past_written_reads_nothing() {
  FollowerLogRing r;
  r.append("new\n", 4);
  uint32_t cursor = 9999;
  TEST_ASSERT_EQUAL_STRING("", drain(r, cursor).c_str());
}

static void test_every_cursor_reads_the_same_tail() {
  // Fill past a wrap in small steps; from every cursor a reader could hold,
  // what comes out is the ring's content from that point to its last newline.
  FollowerLogRing r;
  String all;
  for (int step = 0; step < 9; step++) {
    r.append("abcde\n", 6);
    all += "abcde\n";
    for (uint32_t after = 0; after <= r.written + 3; after++) {
      uint32_t from = after < r.oldestCursor() ? r.oldestCursor() : after;
      if (from > r.written) from = r.written;
      uint32_t cursor = after;
      const String got = drain(r, cursor);
      TEST_ASSERT_EQUAL_STRING(all.substring(from).c_str(), got.c_str());
    }
  }
}

// --- uptime stamps (#503) ---

static void test_every_line_opens_with_its_uptime_stamp() {
  FollowerLogRing r;
  r.appendStamped("a\n", 2, 0);
  r.appendStamped("b", 1, 7);
  r.appendStamped("c\n", 2, 8);  // same line, written in two calls
  uint32_t cursor = 0;
  TEST_ASSERT_EQUAL_STRING("[0] a\n[7] bc\n", drain(r, cursor).c_str());
}

static void test_stamp_survives_the_largest_uptime() {
  FollowerLogRing r;
  r.appendStamped("z\n", 2, 0xFFFFFFFFUL);
  uint32_t cursor = 0;
  TEST_ASSERT_EQUAL_STRING("[4294967295] z\n", drain(r, cursor).c_str());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_fresh_ring_has_no_line);
  RUN_TEST(test_lines_come_whole_without_the_newline);
  RUN_TEST(test_only_new_lines_follow_a_cursor);
  RUN_TEST(test_a_line_still_being_written_waits);
  RUN_TEST(test_an_empty_line_is_sent_as_empty);
  RUN_TEST(test_a_line_longer_than_the_message_comes_in_pieces);
  RUN_TEST(test_wrap_evicts_oldest_but_cursor_stays_monotonic);
  RUN_TEST(test_stale_evicted_cursor_clamps_to_oldest_retained);
  RUN_TEST(test_cursor_past_written_reads_nothing);
  RUN_TEST(test_every_cursor_reads_the_same_tail);
  RUN_TEST(test_every_line_opens_with_its_uptime_stamp);
  RUN_TEST(test_stamp_survives_the_largest_uptime);
  return UNITY_END();
}
