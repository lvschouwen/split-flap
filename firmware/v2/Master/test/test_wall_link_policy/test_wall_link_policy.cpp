// Native tests for WallLinkPolicy.h: the master's rules for a row board's
// connection on the wall link.
#include <unity.h>

#include "WallLinkPolicy.h"

void setUp() {}
void tearDown() {}

static WallRowContact connectedAt(uint32_t ms) {
  WallRowContact c;
  wallRowConnected(c, ms);
  return c;
}

// ---- ping ----------------------------------------------------------------------

static void test_ping_only_after_ten_seconds_without_sending() {
  WallRowContact c = connectedAt(1000);
  c.helloSeen = true;
  wallRowSent(c, 1000);
  TEST_ASSERT_FALSE(wallLinkPingDue(c, 10999));
  TEST_ASSERT_TRUE(wallLinkPingDue(c, 11000));
  wallRowSent(c, 11000);
  TEST_ASSERT_FALSE(wallLinkPingDue(c, 12000));
}

static void test_no_ping_while_the_row_is_busy_or_before_its_hello() {
  WallRowContact c = connectedAt(0);
  TEST_ASSERT_FALSE(wallLinkPingDue(c, 60000));  // no Hello yet
  c.helloSeen = true;
  TEST_ASSERT_TRUE(wallLinkPingDue(c, 60000));
  c.busy = true;
  TEST_ASSERT_FALSE(wallLinkPingDue(c, 60000));
}

static void test_ping_timing_survives_the_millisecond_counter_wrapping() {
  WallRowContact c = connectedAt(0xFFFFFF00u);
  c.helloSeen = true;
  wallRowSent(c, 0xFFFFFF00u);
  TEST_ASSERT_FALSE(wallLinkPingDue(c, 100));    // 356 ms later
  TEST_ASSERT_TRUE(wallLinkPingDue(c, 10000));   // 10256 ms later
}

// ---- hello ---------------------------------------------------------------------

static void test_a_connection_that_says_nothing_is_closed_after_five_seconds() {
  WallRowContact c = connectedAt(2000);
  TEST_ASSERT_FALSE(wallLinkHelloOverdue(c, 6999));
  TEST_ASSERT_TRUE(wallLinkHelloOverdue(c, 7000));
  c.helloSeen = true;
  TEST_ASSERT_FALSE(wallLinkHelloOverdue(c, 60000));
}

static void test_hello_is_taken_only_from_a_known_row_on_this_protocol() {
  TEST_ASSERT_TRUE(WallHello::Accept == wallLinkJudgeHello(WALL_LINK_PROTOCOL, "row-a", true));
  TEST_ASSERT_TRUE(WallHello::WrongProtocol ==
                   wallLinkJudgeHello(WALL_LINK_PROTOCOL + 1, "row-a", true));
  TEST_ASSERT_TRUE(WallHello::NoId == wallLinkJudgeHello(WALL_LINK_PROTOCOL, "", true));
  TEST_ASSERT_TRUE(WallHello::NotPaired == wallLinkJudgeHello(WALL_LINK_PROTOCOL, "row-b", false));
}

// ---- reach ---------------------------------------------------------------------

static void test_a_row_never_heard_is_absent_not_lost() {
  WallRowContact c;
  TEST_ASSERT_TRUE(WallRowReach::Never == wallRowReach(c, 500000));
}

static void test_a_row_is_up_while_messages_arrive_and_lost_after_thirty_seconds() {
  WallRowContact c = connectedAt(0);
  wallRowHeard(c, 1000);
  TEST_ASSERT_TRUE(WallRowReach::Up == wallRowReach(c, 30999));
  TEST_ASSERT_TRUE(WallRowReach::Lost == wallRowReach(c, 31000));
  wallRowHeard(c, 31000);
  TEST_ASSERT_TRUE(WallRowReach::Up == wallRowReach(c, 31001));
}

static void test_a_dropped_connection_is_away_until_the_thirty_seconds_pass() {
  WallRowContact c = connectedAt(0);
  wallRowHeard(c, 5000);
  wallRowDropped(c);
  TEST_ASSERT_TRUE(WallRowReach::Away == wallRowReach(c, 6000));
  TEST_ASSERT_TRUE(WallRowReach::Away == wallRowReach(c, 34999));
  TEST_ASSERT_TRUE(WallRowReach::Lost == wallRowReach(c, 35000));
}

// A row in a unit job does not service its link (12 s for one unit update, 29 s
// for a self-test, minutes for a whole row): silence there is expected. A busy
// row that lost its power is found by the connection's keepalive instead, which
// the row's TCP stack answers during a job.
static void test_a_busy_row_is_not_lost_by_silence_while_its_connection_stands() {
  WallRowContact c = connectedAt(0);
  wallRowHeard(c, 1000);
  c.busy = true;
  TEST_ASSERT_TRUE(WallRowReach::Busy == wallRowReach(c, 400000));
  wallRowDropped(c);
  TEST_ASSERT_TRUE(WallRowReach::Lost == wallRowReach(c, 400000));
}

static void test_a_new_connection_starts_clean() {
  WallRowContact c = connectedAt(0);
  c.helloSeen = true;
  c.busy = true;
  wallRowDropped(c);
  TEST_ASSERT_FALSE(c.connected);
  TEST_ASSERT_FALSE(c.busy);
  wallRowConnected(c, 9000);
  TEST_ASSERT_TRUE(c.connected);
  TEST_ASSERT_FALSE(c.helloSeen);
  // Connecting is not contact: only a message is.
  TEST_ASSERT_TRUE(WallRowReach::Never == wallRowReach(connectedAt(9000), 9500));
}

static void test_keepalive_gives_up_at_the_lost_mark() {
  TEST_ASSERT_EQUAL_UINT32(WALL_LINK_LOST_MS / 1000,
                           WALL_LINK_KEEPALIVE_IDLE_S +
                               WALL_LINK_KEEPALIVE_INTERVAL_S * WALL_LINK_KEEPALIVE_COUNT);
}

// ---- restart -------------------------------------------------------------------

static void test_a_new_boot_id_means_the_row_restarted() {
  WallRowContact c;
  TEST_ASSERT_FALSE(wallRowNoteBoot(c, 77));  // first Hello ever: nothing was open
  TEST_ASSERT_FALSE(wallRowNoteBoot(c, 77));  // same start, new connection
  TEST_ASSERT_TRUE(wallRowNoteBoot(c, 78));
  TEST_ASSERT_FALSE(wallRowNoteBoot(c, 78));
  WallRowContact zero;
  TEST_ASSERT_FALSE(wallRowNoteBoot(zero, 0));
  TEST_ASSERT_TRUE(wallRowNoteBoot(zero, 5));  // a boot id of 0 is an id like any other
}

// ---- text: the latest only -------------------------------------------------------

static void test_only_the_latest_text_is_kept_for_a_row() {
  WallRowText t;
  TEST_ASSERT_FALSE(wallRowTextDue(t, connectedAt(0)));
  wallRowTextSet(t, "FIRST", 80, 1000);
  wallRowTextSet(t, "SECOND", 60, 2000);
  WallRowContact c = connectedAt(0);
  c.helloSeen = true;
  TEST_ASSERT_TRUE(wallRowTextDue(t, c));
  TEST_ASSERT_EQUAL_STRING("SECOND", t.text);
  TEST_ASSERT_EQUAL_UINT32(2, t.renderId);
  TEST_ASSERT_EQUAL_UINT16(60, t.speed);
  TEST_ASSERT_TRUE(2000 == t.commitAtMs);
  wallRowTextSent(t);
  TEST_ASSERT_FALSE(wallRowTextDue(t, c));
}

static void test_a_text_waits_for_hello_and_for_a_busy_row() {
  WallRowText t;
  wallRowTextSet(t, "HELLO", 80, 0);
  WallRowContact c;
  TEST_ASSERT_FALSE(wallRowTextDue(t, c));  // not connected
  wallRowConnected(c, 0);
  TEST_ASSERT_FALSE(wallRowTextDue(t, c));  // no Hello yet
  c.helloSeen = true;
  c.busy = true;
  TEST_ASSERT_FALSE(wallRowTextDue(t, c));
  c.busy = false;
  TEST_ASSERT_TRUE(wallRowTextDue(t, c));
}

static void test_the_same_text_again_is_not_a_new_render() {
  WallRowText t;
  TEST_ASSERT_TRUE(wallRowTextSet(t, "SAME", 80, 0));
  wallRowTextSent(t);
  TEST_ASSERT_FALSE(wallRowTextSet(t, "SAME", 80, 5000));
  TEST_ASSERT_EQUAL_UINT32(1, t.renderId);
  TEST_ASSERT_FALSE(t.pending);
  TEST_ASSERT_TRUE(wallRowTextSet(t, "SAME", 40, 0));  // another speed is another render
  TEST_ASSERT_EQUAL_UINT32(2, t.renderId);
}

// A row that reconnects may have fallen back to its own clock, or restarted
// blank: it gets the current text again, to flip on arrival.
static void test_a_reconnected_row_gets_the_current_text_at_once() {
  WallRowText t;
  wallRowTextSet(t, "NOW", 80, 123456);
  wallRowTextSent(t);
  wallRowTextResend(t);
  WallRowContact c = connectedAt(0);
  c.helloSeen = true;
  TEST_ASSERT_TRUE(wallRowTextDue(t, c));
  TEST_ASSERT_TRUE(0 == t.commitAtMs);
  TEST_ASSERT_EQUAL_UINT32(2, t.renderId);
  WallRowText empty;
  wallRowTextResend(empty);
  TEST_ASSERT_FALSE(empty.pending);  // nothing was ever shown: nothing to repeat
}

static void test_a_text_too_long_for_the_message_is_cut_to_a_row() {
  WallRowText t;
  wallRowTextSet(t, "ABCDEFGHIJKLMNOPQRSTUVWXYZ", 80, 0);
  TEST_ASSERT_EQUAL_STRING("ABCDEFGHIJKLMNOP", t.text);
}

static void test_shown_is_matched_to_the_latest_render_only() {
  WallRowText t;
  wallRowTextSet(t, "ONE", 80, 0);
  wallRowTextSent(t);
  wallRowTextSet(t, "TWO", 80, 0);
  wallRowTextSent(t);
  wallRowTextShown(t, 1);
  TEST_ASSERT_FALSE(wallRowTextApplied(t));
  wallRowTextShown(t, 2);
  TEST_ASSERT_TRUE(wallRowTextApplied(t));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_ping_only_after_ten_seconds_without_sending);
  RUN_TEST(test_no_ping_while_the_row_is_busy_or_before_its_hello);
  RUN_TEST(test_ping_timing_survives_the_millisecond_counter_wrapping);
  RUN_TEST(test_a_connection_that_says_nothing_is_closed_after_five_seconds);
  RUN_TEST(test_hello_is_taken_only_from_a_known_row_on_this_protocol);
  RUN_TEST(test_a_row_never_heard_is_absent_not_lost);
  RUN_TEST(test_a_row_is_up_while_messages_arrive_and_lost_after_thirty_seconds);
  RUN_TEST(test_a_dropped_connection_is_away_until_the_thirty_seconds_pass);
  RUN_TEST(test_a_busy_row_is_not_lost_by_silence_while_its_connection_stands);
  RUN_TEST(test_a_new_connection_starts_clean);
  RUN_TEST(test_keepalive_gives_up_at_the_lost_mark);
  RUN_TEST(test_a_new_boot_id_means_the_row_restarted);
  RUN_TEST(test_only_the_latest_text_is_kept_for_a_row);
  RUN_TEST(test_a_text_waits_for_hello_and_for_a_busy_row);
  RUN_TEST(test_the_same_text_again_is_not_a_new_render);
  RUN_TEST(test_a_reconnected_row_gets_the_current_text_at_once);
  RUN_TEST(test_a_text_too_long_for_the_message_is_cut_to_a_row);
  RUN_TEST(test_shown_is_matched_to_the_latest_render_only);
  return UNITY_END();
}
