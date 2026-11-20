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
#include "../../FollowerEscalation.h"

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

// --- /log reader (#679) -------------------------------------------------------------

// The whole stream: chunks of `cap` from offset 0 until the reader ends.
static String stream(const FollowerLogRing& r, const FollowerLogRing::Snapshot& snap,
                     size_t cap, bool* overtakenOut = nullptr) {
  String all;
  char out[32];
  size_t sent = 0;
  for (;;) {
    bool over = false;
    size_t n = r.readSnapshot(snap, sent, out, cap, over);
    if (overtakenOut != nullptr && over) *overtakenOut = true;
    if (n == 0) break;
    for (size_t i = 0; i < n; i++) all += out[i];
    sent += n;
  }
  return all;
}

static void test_reader_streams_all_without_moving_the_link_cursor() {
  FollowerLogRing r;
  r.append("one\ntwo\n", 8);
  uint32_t linkCursor = 4;  // the wall link has had "one"
  auto snap = r.snapshot();
  TEST_ASSERT_EQUAL_STRING("one\ntwo\n", stream(r, snap, 3).c_str());
  TEST_ASSERT_EQUAL_UINT32(4, linkCursor);
  // The link still gets "two" afterwards: reading /log took nothing from it.
  TEST_ASSERT_EQUAL_STRING("two\n", drain(r, linkCursor).c_str());
}

static void test_reader_on_an_empty_ring_ends_at_once() {
  FollowerLogRing r;
  TEST_ASSERT_EQUAL_STRING("", stream(r, r.snapshot(), 8).c_str());
}

static void test_reader_wraps_and_opens_on_a_whole_line() {
  FollowerLogRing r;  // 16 bytes
  r.append("aaaa\nbbbb\n", 10);
  r.append("cc\ndd\n", 7);  // 17 written: "aaaa\n" is cut, 'a' x1 gone
  auto snap = r.snapshot();
  // the cut first line is skipped, the rest comes whole across the wrap
  TEST_ASSERT_EQUAL_STRING("bbbb\ncc\ndd\n", stream(r, snap, 5).c_str());
}

static void test_reader_ignores_lines_appended_after_the_snapshot() {
  FollowerLogRing r;
  r.append("one\n", 4);
  auto snap = r.snapshot();
  r.append("two\n", 4);
  TEST_ASSERT_EQUAL_STRING("one\n", stream(r, snap, 8).c_str());
}

static void test_reader_overtaken_mid_stream_stops_cleanly() {
  FollowerLogRing r;
  r.append("aaa\nbbb\nccc\n", 12);
  auto snap = r.snapshot();
  char out[32];
  bool over = false;
  TEST_ASSERT_EQUAL_size_t(4, r.readSnapshot(snap, 0, out, 4, over));
  TEST_ASSERT_FALSE(over);
  r.append("dddddddd\n", 9);  // 21 written: the ring now starts past byte 4
  TEST_ASSERT_EQUAL_size_t(0, r.readSnapshot(snap, 4, out, 4, over));
  TEST_ASSERT_TRUE(over);
}

static void test_reader_untouched_by_appends_that_keep_its_bytes() {
  FollowerLogRing r;
  r.append("aaa\nbbb\n", 8);
  auto snap = r.snapshot();
  char out[32];
  bool over = false;
  r.append("c\n", 2);  // 10 written, nothing evicted yet
  TEST_ASSERT_EQUAL_size_t(4, r.readSnapshot(snap, 4, out, 8, over));
  TEST_ASSERT_FALSE(over);
  TEST_ASSERT_EQUAL_MEMORY("bbb\n", out, 4);
}

static void test_one_unfinished_line_in_a_full_ring_reads_nothing() {
  FollowerLogRing r;
  r.append("abcdefghijklmnopqrs", 19);  // wrapped, no newline held
  TEST_ASSERT_EQUAL_STRING("", stream(r, r.snapshot(), 8).c_str());
}

static void test_serve_floor() {
  TEST_ASSERT_FALSE(followerLogServeAllowed(FOLLOWER_LOG_SERVE_MIN_BLOCK - 1));
  TEST_ASSERT_TRUE(followerLogServeAllowed(FOLLOWER_LOG_SERVE_MIN_BLOCK));
  // never below the block FollowerEscalation.h counts as low
  TEST_ASSERT_TRUE(FOLLOWER_LOG_SERVE_MIN_BLOCK > ESCALATION_LOW_BLOCK_BYTES);
}

static void test_overtaken_stream_ends_with_the_marker_once() {
  FollowerLogRing r;
  r.append("aaa\nbbb\nccc\n", 12);
  auto snap = r.snapshot();
  FollowerLogStream st;
  char out[32];
  size_t sent = 0;
  String all;
  size_t n = followerLogStreamRead(r, snap, st, sent, out, 4);  // "aaa\n"
  for (size_t i = 0; i < n; i++) all += out[i];
  sent += n;
  r.append("dddddddd\n", 9);  // the ring moves past the reader
  for (;;) {
    n = followerLogStreamRead(r, snap, st, sent, out, 5);  // marker in pieces
    if (n == 0) break;
    for (size_t i = 0; i < n; i++) all += out[i];
    sent += n;
  }
  TEST_ASSERT_EQUAL_STRING("aaa\n\n[log overtaken]\n", all.c_str());
  TEST_ASSERT_EQUAL_size_t(0, followerLogStreamRead(r, snap, st, sent, out, 5));
}

static void test_a_stream_not_overtaken_has_no_marker() {
  FollowerLogRing r;
  r.append("one\n", 4);
  auto snap = r.snapshot();
  FollowerLogStream st;
  char out[32];
  TEST_ASSERT_EQUAL_size_t(4, followerLogStreamRead(r, snap, st, 0, out, 32));
  TEST_ASSERT_EQUAL_size_t(0, followerLogStreamRead(r, snap, st, 4, out, 32));
  TEST_ASSERT_FALSE(st.overtaken);
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
  RUN_TEST(test_reader_streams_all_without_moving_the_link_cursor);
  RUN_TEST(test_reader_on_an_empty_ring_ends_at_once);
  RUN_TEST(test_reader_wraps_and_opens_on_a_whole_line);
  RUN_TEST(test_reader_ignores_lines_appended_after_the_snapshot);
  RUN_TEST(test_reader_overtaken_mid_stream_stops_cleanly);
  RUN_TEST(test_reader_untouched_by_appends_that_keep_its_bytes);
  RUN_TEST(test_one_unfinished_line_in_a_full_ring_reads_nothing);
  RUN_TEST(test_serve_floor);
  RUN_TEST(test_overtaken_stream_ends_with_the_marker_once);
  RUN_TEST(test_a_stream_not_overtaken_has_no_marker);
  return UNITY_END();
}
