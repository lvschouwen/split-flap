// Native tests for FollowerLinkPolicy.h: the timing and pairing rules of the
// row board's wall link.
#include <unity.h>

#include "FollowerLinkPolicy.h"

void setUp() {}
void tearDown() {}

static void test_backoff_starts_at_one_second_and_stops_at_eight() {
  uint32_t b = followerLinkNextBackoffMs(0);
  TEST_ASSERT_EQUAL_UINT32(1000, b);
  b = followerLinkNextBackoffMs(b);
  TEST_ASSERT_EQUAL_UINT32(2000, b);
  b = followerLinkNextBackoffMs(b);
  TEST_ASSERT_EQUAL_UINT32(4000, b);
  b = followerLinkNextBackoffMs(b);
  TEST_ASSERT_EQUAL_UINT32(8000, b);
  TEST_ASSERT_EQUAL_UINT32(8000, followerLinkNextBackoffMs(b));
}

static void test_elapsed_survives_the_millisecond_counter_wrapping() {
  TEST_ASSERT_FALSE(followerLinkElapsed(5000, 1000, 10000));
  TEST_ASSERT_TRUE(followerLinkElapsed(11000, 1000, 10000));
  TEST_ASSERT_FALSE(followerLinkElapsed(100, 0xFFFFFF00u, 10000));   // 356 ms later
  TEST_ASSERT_TRUE(followerLinkElapsed(10000, 0xFFFFFF00u, 10000));  // 10256 ms later
}

static void test_host_part_drops_a_port_from_the_stored_address() {
  char host[48];
  TEST_ASSERT_TRUE(followerLinkHostPart("192.168.1.42", host, sizeof host));
  TEST_ASSERT_EQUAL_STRING("192.168.1.42", host);
  TEST_ASSERT_TRUE(followerLinkHostPart("192.168.1.42:8801", host, sizeof host));
  TEST_ASSERT_EQUAL_STRING("192.168.1.42", host);
  TEST_ASSERT_FALSE(followerLinkHostPart("", host, sizeof host));
  TEST_ASSERT_FALSE(followerLinkHostPart(":8801", host, sizeof host));
  char tiny[8];
  TEST_ASSERT_FALSE(followerLinkHostPart("192.168.1.42", tiny, sizeof tiny));
}

static void test_only_the_paired_master_is_accepted() {
  TEST_ASSERT_TRUE(followerLinkMasterAccepted("split-flap-c8a746", "split-flap-c8a746"));
  TEST_ASSERT_FALSE(followerLinkMasterAccepted("split-flap-c8a746", "split-flap-a47dee"));
  TEST_ASSERT_FALSE(followerLinkMasterAccepted("", ""));  // not paired: nobody is the master
  TEST_ASSERT_FALSE(followerLinkMasterAccepted("split-flap-c8a746", ""));
}

static void test_what_can_wait_is_written_only_with_room_left_for_an_answer() {
  // The whole message and the reserve must fit in what the send buffer has free.
  TEST_ASSERT_TRUE(followerLinkRoomFor(500 + FOLLOWER_LINK_REPLY_RESERVE, 500));
  TEST_ASSERT_FALSE(followerLinkRoomFor(500 + FOLLOWER_LINK_REPLY_RESERVE - 1, 500));
  TEST_ASSERT_FALSE(followerLinkRoomFor(0, 1));
}

static void test_a_master_that_says_nothing_for_three_pings_is_gone() {
  TEST_ASSERT_FALSE(followerLinkMasterSilent(1000 + FOLLOWER_LINK_SILENT_MS - 1, 1000));
  TEST_ASSERT_TRUE(followerLinkMasterSilent(1000 + FOLLOWER_LINK_SILENT_MS, 1000));
  TEST_ASSERT_FALSE(followerLinkMasterSilent(5, 0xFFFFFFF0UL));  // across the wrap
}

static void test_a_pass_that_comes_late_starts_the_silence_count_again() {
  // loop() stood still (a unit update, a download): what the master sent in
  // that time waits unread, so it was not silent.
  const uint32_t heard = 1000, lastPass = 20000;
  TEST_ASSERT_EQUAL_UINT32(heard, followerLinkHeardAfterPass(heard, lastPass, lastPass + 10, false));
  TEST_ASSERT_EQUAL_UINT32(
      heard, followerLinkHeardAfterPass(heard, lastPass, lastPass + FOLLOWER_LINK_STALL_MS - 1, false));
  const uint32_t late = lastPass + FOLLOWER_LINK_STALL_MS;
  TEST_ASSERT_EQUAL_UINT32(late, followerLinkHeardAfterPass(heard, lastPass, late, false));
}

static void test_a_busy_row_does_not_count_its_master_silent() {
  // The master does not ping a row that reports itself busy.
  TEST_ASSERT_EQUAL_UINT32(20010, followerLinkHeardAfterPass(1000, 20000, 20010, true));
}

static void test_unit_facts_go_up_on_the_timer_or_when_a_job_asks() {
  TEST_ASSERT_FALSE(followerLinkUnitsDue(20000, 0, false, 7, 7));
  TEST_ASSERT_TRUE(followerLinkUnitsDue(30000, 0, false, 7, 7));
  TEST_ASSERT_TRUE(followerLinkUnitsDue(1, 0, true, 7, 7));
}

static void test_a_settled_render_sends_the_unit_facts_without_waiting_for_the_timer() {
  TEST_ASSERT_TRUE(followerLinkUnitsDue(12000, 10000, false, 8, 7));
}

static void test_settled_renders_in_quick_succession_send_once_per_gap() {
  // A render settled 1 s after the last document went up: it waits for the
  // gap, then goes up without another render.
  TEST_ASSERT_FALSE(followerLinkUnitsDue(11000, 10000, false, 8, 7));
  TEST_ASSERT_TRUE(followerLinkUnitsDue(10000 + FOLLOWER_LINK_UNITS_MIN_GAP_MS, 10000, false, 8, 7));
}

static void test_the_render_count_wrapping_still_counts_as_a_new_render() {
  TEST_ASSERT_TRUE(followerLinkUnitsDue(12000, 10000, false, 0, 0xFFFFFFFFu));
}

static const size_t BUF = 1072;

static void test_a_hello_left_unacknowledged_is_an_uplink_failure() {
  FollowerUplinkAck a;
  TEST_ASSERT_TRUE(followerLinkUplinkFailed(FollowerLinkClose::NoWelcome, 41, a));
  // Acknowledged but unanswered: the master heard the row and chose not to talk.
  TEST_ASSERT_FALSE(followerLinkUplinkFailed(FollowerLinkClose::NoWelcome, 0, a));
  TEST_ASSERT_FALSE(followerLinkUplinkFailed(FollowerLinkClose::NoWelcome, -1, a));
}

// A welcomed connection: Hello written at 1000 and acknowledged, master heard.
static FollowerUplinkAck healthyConnection() {
  FollowerUplinkAck a;
  followerLinkNoteOpened(a, BUF);
  followerLinkNoteWrite(a, BUF - 41, BUF, 1000);
  followerLinkNoteSendBuffer(a, BUF, BUF, 1030, 1030, true);
  return a;
}

static void test_a_close_the_master_chose_owes_nothing() {
  // A master restart or OTA: the connection was healthy up to its end.
  FollowerUplinkAck a = healthyConnection();
  followerLinkNoteWrite(a, BUF - 60, BUF, 10000);
  followerLinkNoteSendBuffer(a, BUF, BUF, 10030, 10030, true);
  TEST_ASSERT_FALSE(followerLinkUplinkFailed(FollowerLinkClose::Lost, -1, a));
  TEST_ASSERT_FALSE(followerLinkUplinkFailed(FollowerLinkClose::WriteFailed, 900, a));
  TEST_ASSERT_FALSE(followerLinkUplinkFailed(FollowerLinkClose::Plain, 500, a));
}

static void test_bytes_unacknowledged_while_the_master_is_heard_are_a_stall() {
  FollowerUplinkAck a = healthyConnection();
  followerLinkNoteWrite(a, BUF - 60, BUF, 10000);
  followerLinkNoteSendBuffer(a, BUF - 60, BUF, 15000, 15000, true);  // a Ping arrived
  followerLinkNoteSendBuffer(a, BUF - 60, BUF, 19999, 15000, true);
  TEST_ASSERT_FALSE(a.stalled);
  followerLinkNoteSendBuffer(a, BUF - 60, BUF, 10000 + FOLLOWER_LINK_UPLINK_STALL_MS, 15000, true);
  TEST_ASSERT_TRUE(a.stalled);
  TEST_ASSERT_TRUE(followerLinkUplinkFailed(FollowerLinkClose::Lost, -1, a));
  TEST_ASSERT_TRUE(followerLinkUplinkFailed(FollowerLinkClose::WriteFailed, 60, a));
  followerLinkNoteClosed(a);
  TEST_ASSERT_FALSE(followerLinkUplinkFailed(FollowerLinkClose::Lost, -1, a));
}

static void test_a_stall_without_the_master_heard_is_no_uplink_evidence() {
  // Both directions gone: nothing says it is the uplink.
  FollowerUplinkAck a = healthyConnection();
  followerLinkNoteWrite(a, BUF - 60, BUF, 10000);
  followerLinkNoteSendBuffer(a, BUF - 60, BUF, 30000, 1030, true);
  TEST_ASSERT_FALSE(a.stalled);
}

static void test_a_stall_is_not_measured_across_a_stood_still_or_busy_loop() {
  TEST_ASSERT_TRUE(followerLinkPassMeasurable(10000, 10010, false));
  TEST_ASSERT_FALSE(followerLinkPassMeasurable(10000, 10000 + FOLLOWER_LINK_STALL_MS, false));
  TEST_ASSERT_FALSE(followerLinkPassMeasurable(10000, 10010, true));
  FollowerUplinkAck a = healthyConnection();
  followerLinkNoteWrite(a, BUF - 60, BUF, 10000);
  // A unit job held the row to 25000; the master's Ping then counts as heard.
  followerLinkNoteSendBuffer(a, BUF - 60, BUF, 25000, 25000, false);
  followerLinkNoteSendBuffer(a, BUF - 60, BUF, 25010, 25000, true);
  TEST_ASSERT_FALSE(a.stalled);
  followerLinkNoteSendBuffer(a, BUF - 60, BUF, 25000 + FOLLOWER_LINK_UPLINK_STALL_MS, 25000, true);
  TEST_ASSERT_TRUE(a.stalled);
}

static void test_a_chatty_link_that_is_acknowledged_never_stalls_and_stays_confirmed() {
  // A write every pass keeps the buffer from ever draining; the free bytes
  // still grow between looks as the master acknowledges.
  FollowerUplinkAck a = healthyConnection();
  for (uint32_t t = 2000; t < 60000; t += 50) {
    followerLinkNoteWrite(a, BUF - 400, BUF, t);
    followerLinkNoteSendBuffer(a, BUF - 200, BUF, t + 40, t, true);
  }
  TEST_ASSERT_FALSE(a.stalled);
  TEST_ASSERT_TRUE(followerLinkUplinkConfirmed(a, 60000));
}

// The ladder online at `index`, past its settle window, link up.
static WifiTxInput onlineInput() {
  WifiTxInput in;
  in.phase = WifiTxPhase::Online;
  in.linkUp = true;
  in.rssiDbm = -40;  // strong: no weak-signal step of its own
  in.trafficConfirmed = true;
  return in;
}

static WifiTxState onlineLadderAt(uint8_t index, uint32_t& nowMs) {
  WifiTxState st;
  nowMs = 1000;
  wifiTxPolicyStep(st, onlineInput(), nowMs);
  st.index = index;
  nowMs += WIFI_TX_SETTLE_MS;
  return st;
}

static void test_an_uplink_failure_earns_one_level_and_keeps_it() {
  uint32_t now = 0;
  WifiTxState st = onlineLadderAt(0, now);
  TEST_ASSERT_TRUE(followerLinkOweTxStep(st));
  const WifiTxInput in = onlineInput();
  TEST_ASSERT_EQUAL_UINT8(1, wifiTxPolicyStep(st, in, now));
  TEST_ASSERT_TRUE(st.lastReason == WifiTxStepReason::LinkDrop);
  TEST_ASSERT_EQUAL_UINT8(1, st.dropFloor);
  // A strong signal for longer than the healthy period does not hand it back.
  TEST_ASSERT_EQUAL_UINT8(1, wifiTxPolicyStep(st, in, now + 1000));
  TEST_ASSERT_EQUAL_UINT8(1, wifiTxPolicyStep(st, in, now + 2000 + WIFI_TX_HEALTHY_MS));
}

static void test_a_run_of_uplink_failures_climbs_one_level_per_dwell() {
  uint32_t now = 0;
  WifiTxState st = onlineLadderAt(0, now);
  const WifiTxInput in = onlineInput();
  for (int i = 0; i < 10; i++) {  // ten evidence closes, a second apart
    followerLinkOweTxStep(st);
    wifiTxPolicyStep(st, in, now + (uint32_t)i * 1000);
  }
  TEST_ASSERT_EQUAL_UINT8(1, st.index);
  TEST_ASSERT_EQUAL_UINT16(1, st.stepsUp);
  TEST_ASSERT_EQUAL_UINT8(2, wifiTxPolicyStep(st, in, now + WIFI_TX_UP_DWELL_MS));
}

static void test_an_uplink_failure_at_the_ceiling_owes_nothing() {
  uint32_t now = 0;
  WifiTxState st = onlineLadderAt(WIFI_TX_ONLINE_CEILING_INDEX, now);
  TEST_ASSERT_FALSE(followerLinkOweTxStep(st));
  TEST_ASSERT_FALSE(st.dropPending);
}

static void test_only_an_acknowledgement_after_a_write_confirms_the_uplink() {
  FollowerUplinkAck a;
  followerLinkNoteOpened(a, BUF);
  TEST_ASSERT_FALSE(followerLinkUplinkConfirmed(a, 1000));
  followerLinkNoteSendBuffer(a, BUF, BUF, 1000, 1000, true);  // nothing written yet
  TEST_ASSERT_FALSE(followerLinkUplinkConfirmed(a, 1000));
  followerLinkNoteWrite(a, BUF - 41, BUF, 1500);
  followerLinkNoteSendBuffer(a, BUF - 41, BUF, 2000, 2000, true);  // 41 B still in flight
  TEST_ASSERT_FALSE(followerLinkUplinkConfirmed(a, 2000));
  followerLinkNoteSendBuffer(a, BUF, BUF, 3000, 2000, true);
  TEST_ASSERT_TRUE(followerLinkUplinkConfirmed(a, 3000));
  TEST_ASSERT_TRUE(followerLinkUplinkConfirmed(a, 3000 + WIFI_TX_CONFIRM_FRESH_MS - 1));
  TEST_ASSERT_FALSE(followerLinkUplinkConfirmed(a, 3000 + WIFI_TX_CONFIRM_FRESH_MS));
}

static void test_a_closed_connection_takes_the_confirmation_with_it() {
  FollowerUplinkAck a = healthyConnection();
  TEST_ASSERT_TRUE(followerLinkUplinkConfirmed(a, 2000));
  followerLinkNoteClosed(a);
  TEST_ASSERT_FALSE(followerLinkUplinkConfirmed(a, 2000));
  followerLinkNoteOpened(a, BUF);
  followerLinkNoteSendBuffer(a, BUF, BUF, 5000, 5000, true);  // the next connection's empty buffer
  TEST_ASSERT_FALSE(followerLinkUplinkConfirmed(a, 5000));
}

static void test_an_acknowledgement_at_millis_zero_still_counts() {
  FollowerUplinkAck a;
  followerLinkNoteOpened(a, BUF);
  followerLinkNoteWrite(a, BUF - 41, BUF, 0);
  followerLinkNoteSendBuffer(a, BUF, BUF, 0, 0, true);
  TEST_ASSERT_TRUE(followerLinkUplinkConfirmed(a, 10));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_backoff_starts_at_one_second_and_stops_at_eight);
  RUN_TEST(test_elapsed_survives_the_millisecond_counter_wrapping);
  RUN_TEST(test_host_part_drops_a_port_from_the_stored_address);
  RUN_TEST(test_only_the_paired_master_is_accepted);
  RUN_TEST(test_what_can_wait_is_written_only_with_room_left_for_an_answer);
  RUN_TEST(test_a_master_that_says_nothing_for_three_pings_is_gone);
  RUN_TEST(test_a_pass_that_comes_late_starts_the_silence_count_again);
  RUN_TEST(test_a_busy_row_does_not_count_its_master_silent);
  RUN_TEST(test_unit_facts_go_up_on_the_timer_or_when_a_job_asks);
  RUN_TEST(test_a_settled_render_sends_the_unit_facts_without_waiting_for_the_timer);
  RUN_TEST(test_settled_renders_in_quick_succession_send_once_per_gap);
  RUN_TEST(test_the_render_count_wrapping_still_counts_as_a_new_render);
  RUN_TEST(test_a_hello_left_unacknowledged_is_an_uplink_failure);
  RUN_TEST(test_a_close_the_master_chose_owes_nothing);
  RUN_TEST(test_bytes_unacknowledged_while_the_master_is_heard_are_a_stall);
  RUN_TEST(test_a_stall_without_the_master_heard_is_no_uplink_evidence);
  RUN_TEST(test_a_stall_is_not_measured_across_a_stood_still_or_busy_loop);
  RUN_TEST(test_a_chatty_link_that_is_acknowledged_never_stalls_and_stays_confirmed);
  RUN_TEST(test_an_uplink_failure_earns_one_level_and_keeps_it);
  RUN_TEST(test_a_run_of_uplink_failures_climbs_one_level_per_dwell);
  RUN_TEST(test_an_uplink_failure_at_the_ceiling_owes_nothing);
  RUN_TEST(test_only_an_acknowledgement_after_a_write_confirms_the_uplink);
  RUN_TEST(test_a_closed_connection_takes_the_confirmation_with_it);
  RUN_TEST(test_an_acknowledgement_at_millis_zero_still_counts);
  return UNITY_END();
}
