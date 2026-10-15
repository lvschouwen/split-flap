// Host-side tests for NetLivenessPolicy.h (#501): when a board that still
// has its WiFi link decides its network is dead and restarts itself.

#include <unity.h>

#include "../../NetLivenessPolicy.h"

void setUp() {}
void tearDown() {}

static const NetProbe OK = NetProbe::Ok;
static const NetProbe FAIL = NetProbe::Fail;
static const NetProbe UNKNOWN = NetProbe::Unknown;

// Runs the policy once a minute from `fromMs` for `minutes`; returns the first
// reboot cause it gives.
static NetLivenessCause run(NetLivenessState& st, uint32_t fromMs, int minutes,
                            bool linkUp, NetProbe gw, NetProbe self,
                            uint8_t strikes = 0) {
  for (int m = 0; m <= minutes; m++) {
    NetLivenessVerdict v =
        netLivenessStep(st, linkUp, gw, self, fromMs + m * 60000UL, strikes);
    if (v.reboot != NetLivenessCause::None) return v.reboot;
  }
  return NetLivenessCause::None;
}

static void test_healthy_network_never_reboots() {
  NetLivenessState st;
  TEST_ASSERT_TRUE(NetLivenessCause::None == run(st, 0, 24 * 60, true, OK, OK));
}

static void test_gateway_lost_after_it_worked_reboots_at_the_threshold() {
  NetLivenessState st;
  run(st, 0, 1, true, OK, OK);
  TEST_ASSERT_TRUE(NetLivenessCause::None == run(st, 120000, 9, true, FAIL, OK));
  NetLivenessVerdict v =
      netLivenessStep(st, true, FAIL, OK, 120000 + NET_LIVENESS_BASE_MS, 0);
  TEST_ASSERT_TRUE(NetLivenessCause::Gateway == v.reboot);
}

static void test_own_server_dead_after_it_worked_reboots() {
  NetLivenessState st;
  run(st, 0, 1, true, OK, OK);
  TEST_ASSERT_TRUE(NetLivenessCause::SelfServer ==
                   run(st, 120000, 11, true, OK, FAIL));
}

// What stops a reboot loop on a dead access point: after the restart nothing
// has been seen working, so nothing can be condemned.
static void test_a_probe_that_never_passed_cannot_cause_a_reboot() {
  NetLivenessState st;
  TEST_ASSERT_TRUE(NetLivenessCause::None ==
                   run(st, 0, 24 * 60, true, FAIL, FAIL));
  // The gateway answers, the own server never did: still no reboot for it.
  NetLivenessState half;
  TEST_ASSERT_TRUE(NetLivenessCause::None ==
                   run(half, 0, 24 * 60, true, OK, FAIL));
}

static void test_one_good_result_restarts_the_clock() {
  NetLivenessState st;
  run(st, 0, 1, true, OK, OK);
  run(st, 120000, 8, true, FAIL, OK);          // 8 min bad
  netLivenessStep(st, true, OK, OK, 660000, 0);  // one good answer
  TEST_ASSERT_TRUE(NetLivenessCause::None == run(st, 720000, 9, true, FAIL, OK));
}

// A link that is down is the WiFi policy's (#328): this one stands down and
// starts from zero when the link returns.
static void test_link_down_is_not_this_watchdogs_business() {
  NetLivenessState st;
  run(st, 0, 1, true, OK, OK);
  run(st, 120000, 8, true, FAIL, OK);
  TEST_ASSERT_TRUE(NetLivenessCause::None ==
                   run(st, 660000, 60, false, FAIL, FAIL));
  TEST_ASSERT_TRUE(NetLivenessCause::None == run(st, 4320000, 9, true, FAIL, OK));
}

// No fresh result is no evidence for the bad clock: the prober may be busy
// with a rollout. That count starts over — an outage is only ever measured
// on unbroken evidence. (Silence for the whole patience is its own verdict
// since #628, tested below; here it stays inside it.)
static void test_unknown_starts_the_count_over() {
  NetLivenessState st;
  run(st, 0, 1, true, OK, OK);
  run(st, 120000, 8, true, FAIL, OK);
  TEST_ASSERT_TRUE(NetLivenessCause::None ==
                   run(st, 660000, 8, true, UNKNOWN, UNKNOWN));
  TEST_ASSERT_FALSE(st.gateway.bad);
  // Failing again afterwards needs the full time once more.
  uint32_t t = 660000 + 9 * 60000UL;
  TEST_ASSERT_TRUE(NetLivenessCause::None == run(st, t, 9, true, FAIL, OK));
  TEST_ASSERT_TRUE(NetLivenessCause::Gateway == run(st, t, 10, true, FAIL, OK));
}

// A router that reboots answers its own port late: what was seen before the
// link dropped proves nothing about the new association.
static void test_a_link_drop_wipes_what_was_seen_working() {
  NetLivenessState st;
  run(st, 0, 1, true, OK, OK);
  netLivenessStep(st, false, UNKNOWN, UNKNOWN, 120000, 0);
  TEST_ASSERT_FALSE(st.gateway.seenOk);
  TEST_ASSERT_FALSE(st.self.seenOk);
  TEST_ASSERT_TRUE(NetLivenessCause::None ==
                   run(st, 180000, 24 * 60, true, FAIL, FAIL));
}

static void test_both_failing_names_the_gateway() {
  NetLivenessState st;
  run(st, 0, 1, true, OK, OK);
  TEST_ASSERT_TRUE(NetLivenessCause::Gateway ==
                   run(st, 120000, 11, true, FAIL, FAIL));
}

// A gateway that never answers on the probed port is inert, not a reason to
// keep the strikes for ever.
static void test_strikes_clear_on_the_own_server_alone() {
  NetLivenessState st;
  NetLivenessVerdict v = netLivenessStep(st, true, FAIL, OK, 1000, 2);
  TEST_ASSERT_FALSE(v.clearStrikes);
  v = netLivenessStep(st, true, FAIL, OK, 1000 + NET_LIVENESS_HEALTHY_RESET_MS, 2);
  TEST_ASSERT_TRUE(v.clearStrikes);
  // ...but not while something that worked is failing.
  NetLivenessState st2;
  netLivenessStep(st2, true, OK, OK, 1000, 2);
  netLivenessStep(st2, true, FAIL, OK, 2000, 2);
  v = netLivenessStep(st2, true, FAIL, OK, 2000 + NET_LIVENESS_HEALTHY_RESET_MS, 2);
  TEST_ASSERT_FALSE(v.clearStrikes);
}

static void test_stale_results_read_as_unknown() {
  TEST_ASSERT_TRUE(OK == netLivenessFresh(OK, 1000, 1000 + NET_LIVENESS_PROBE_STALE_MS));
  TEST_ASSERT_TRUE(UNKNOWN ==
                   netLivenessFresh(FAIL, 1000, 1001 + NET_LIVENESS_PROBE_STALE_MS));
  // millis() wrap between result and now.
  TEST_ASSERT_TRUE(FAIL == netLivenessFresh(FAIL, 0xFFFFFF00UL, 0x00000100UL));
  // The prober stamped its result just after the caller read the clock.
  TEST_ASSERT_TRUE(OK == netLivenessFresh(OK, 5005, 5000));
}

static void test_patience_doubles_per_strike_up_to_a_ceiling() {
  TEST_ASSERT_EQUAL_UINT32(NET_LIVENESS_BASE_MS, netLivenessThresholdMs(0));
  TEST_ASSERT_EQUAL_UINT32(2 * NET_LIVENESS_BASE_MS, netLivenessThresholdMs(1));
  TEST_ASSERT_EQUAL_UINT32(4 * NET_LIVENESS_BASE_MS, netLivenessThresholdMs(2));
  TEST_ASSERT_EQUAL_UINT32(NET_LIVENESS_MAX_MS, netLivenessThresholdMs(10));
  TEST_ASSERT_EQUAL_UINT32(NET_LIVENESS_MAX_MS, netLivenessThresholdMs(255));
  // With one strike the same outage is tolerated twice as long.
  NetLivenessState st;
  run(st, 0, 1, true, OK, OK);
  TEST_ASSERT_TRUE(NetLivenessCause::None ==
                   run(st, 120000, 19, true, FAIL, OK, 1));
  TEST_ASSERT_TRUE(NetLivenessCause::Gateway ==
                   run(st, 120000 + 20 * 60000UL, 1, true, FAIL, OK, 1));
}

static void test_a_healthy_stretch_earns_the_patience_back() {
  NetLivenessState st;
  NetLivenessVerdict v = netLivenessStep(st, true, OK, OK, 1000, 3);
  TEST_ASSERT_FALSE(v.clearStrikes);
  v = netLivenessStep(st, true, OK, OK, 1000 + NET_LIVENESS_HEALTHY_RESET_MS - 1, 3);
  TEST_ASSERT_FALSE(v.clearStrikes);
  v = netLivenessStep(st, true, OK, OK, 1000 + NET_LIVENESS_HEALTHY_RESET_MS, 3);
  TEST_ASSERT_TRUE(v.clearStrikes);
  // A step with nothing answering starts the stretch over.
  netLivenessStep(st, true, UNKNOWN, UNKNOWN, 2000 + NET_LIVENESS_HEALTHY_RESET_MS, 3);
  v = netLivenessStep(st, true, OK, OK, 3000 + NET_LIVENESS_HEALTHY_RESET_MS, 3);
  TEST_ASSERT_FALSE(v.clearStrikes);
}

static void test_bad_clock_survives_millis_wrap() {
  NetLivenessState st;
  netLivenessStep(st, true, OK, OK, 0xFFFF0000UL, 0);
  netLivenessStep(st, true, FAIL, OK, 0xFFFFF000UL, 0);
  NetLivenessVerdict v = netLivenessStep(
      st, true, FAIL, OK, 0xFFFFF000UL + NET_LIVENESS_BASE_MS - 1, 0);
  TEST_ASSERT_TRUE(NetLivenessCause::None == v.reboot);
  v = netLivenessStep(st, true, FAIL, OK, 0xFFFFF000UL + NET_LIVENESS_BASE_MS, 0);
  TEST_ASSERT_TRUE(NetLivenessCause::Gateway == v.reboot);
}

// #628: the wall sat 28 hours with a frozen clock, off MQTT and the LAN,
// while every watchdog stood still. A prober that produces nothing at all
// for the whole patience — no socket to be had, or a task that no longer
// comes round — is a wedged board, not the absence of evidence.
static void test_no_result_at_all_for_the_patience_reboots() {
  NetLivenessState st;
  run(st, 0, 1, true, OK, OK);
  TEST_ASSERT_TRUE(NetLivenessCause::None ==
                   run(st, 120000, 9, true, UNKNOWN, UNKNOWN));
  NetLivenessVerdict v = netLivenessStep(st, true, UNKNOWN, UNKNOWN,
                                         120000 + NET_LIVENESS_BASE_MS, 0);
  TEST_ASSERT_TRUE(NetLivenessCause::NoEvidence == v.reboot);
  // Also from boot: a prober that never produced a first result.
  NetLivenessState fresh;
  TEST_ASSERT_TRUE(NetLivenessCause::NoEvidence ==
                   run(fresh, 0, 11, true, UNKNOWN, UNKNOWN));
}

// Any result, good or bad, is the prober at work: the silence clock restarts.
static void test_any_result_restarts_the_silence_clock() {
  NetLivenessState st;
  run(st, 0, 1, true, OK, OK);
  run(st, 120000, 8, true, UNKNOWN, UNKNOWN);
  netLivenessStep(st, true, FAIL, UNKNOWN, 660000, 0);
  TEST_ASSERT_EQUAL_UINT32(0, st.noEvidenceSinceMs);
  TEST_ASSERT_TRUE(NetLivenessCause::None ==
                   run(st, 720000, 9, true, UNKNOWN, UNKNOWN));
  TEST_ASSERT_TRUE(NetLivenessCause::NoEvidence ==
                   run(st, 720000, 10, true, UNKNOWN, UNKNOWN));
}

// An upload or a release install owns the stack: its silence is on purpose,
// and it does not count once the job is over either.
static void test_paused_probes_are_not_silence() {
  NetLivenessState st;
  run(st, 0, 1, true, OK, OK);
  for (int m = 0; m <= 30; m++) {
    NetLivenessVerdict v = netLivenessStep(st, true, UNKNOWN, UNKNOWN,
                                           120000 + m * 60000UL, 0, true);
    TEST_ASSERT_TRUE(NetLivenessCause::None == v.reboot);
  }
  TEST_ASSERT_EQUAL_UINT32(0, st.noEvidenceSinceMs);
  TEST_ASSERT_TRUE(NetLivenessCause::None ==
                   run(st, 2000000, 9, true, UNKNOWN, UNKNOWN));
}

// Link down belongs to the WiFi policy; the silence clock starts over too.
static void test_link_down_clears_the_silence_clock() {
  NetLivenessState st;
  run(st, 0, 8, true, UNKNOWN, UNKNOWN);
  netLivenessStep(st, false, UNKNOWN, UNKNOWN, 540000, 0);
  TEST_ASSERT_EQUAL_UINT32(0, st.noEvidenceSinceMs);
  TEST_ASSERT_TRUE(NetLivenessCause::None ==
                   run(st, 600000, 9, true, UNKNOWN, UNKNOWN));
}

// The patience is the same one, doubling per strike.
static void test_silence_patience_doubles_per_strike() {
  NetLivenessState st;
  TEST_ASSERT_TRUE(NetLivenessCause::None ==
                   run(st, 1000, 19, true, UNKNOWN, UNKNOWN, 1));
  TEST_ASSERT_TRUE(NetLivenessCause::NoEvidence ==
                   run(st, 1000, 20, true, UNKNOWN, UNKNOWN, 1));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_healthy_network_never_reboots);
  RUN_TEST(test_gateway_lost_after_it_worked_reboots_at_the_threshold);
  RUN_TEST(test_own_server_dead_after_it_worked_reboots);
  RUN_TEST(test_a_probe_that_never_passed_cannot_cause_a_reboot);
  RUN_TEST(test_one_good_result_restarts_the_clock);
  RUN_TEST(test_link_down_is_not_this_watchdogs_business);
  RUN_TEST(test_unknown_starts_the_count_over);
  RUN_TEST(test_a_link_drop_wipes_what_was_seen_working);
  RUN_TEST(test_both_failing_names_the_gateway);
  RUN_TEST(test_strikes_clear_on_the_own_server_alone);
  RUN_TEST(test_stale_results_read_as_unknown);
  RUN_TEST(test_patience_doubles_per_strike_up_to_a_ceiling);
  RUN_TEST(test_a_healthy_stretch_earns_the_patience_back);
  RUN_TEST(test_bad_clock_survives_millis_wrap);
  RUN_TEST(test_no_result_at_all_for_the_patience_reboots);
  RUN_TEST(test_any_result_restarts_the_silence_clock);
  RUN_TEST(test_paused_probes_are_not_silence);
  RUN_TEST(test_link_down_clears_the_silence_clock);
  RUN_TEST(test_silence_patience_doubles_per_strike);
  return UNITY_END();
}
