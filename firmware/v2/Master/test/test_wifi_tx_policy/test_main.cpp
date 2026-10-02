// Host-side unit tests for shared/WifiTxPolicy.h (#507) — the pure TX-power ladder:
// every boot starts at the lowest real level and moves one level at a time,
// up only when the link needs it, never past the phase's ceiling.

#include <ArduinoFake.h>
#include <unity.h>

#include "WifiTxPolicy.h"

void setUp() {}
void tearDown() {}

static const int STRONG_RSSI = -50;  // spare at every level
static const int WEAK_RSSI = -85;    // weak at every level up to the ceiling

static WifiTxInput joining() {
  WifiTxInput in;
  in.phase = WifiTxPhase::Joining;
  return in;
}

static WifiTxInput online(int rssiDbm, bool unitsIdle = true) {
  WifiTxInput in;
  in.phase = WifiTxPhase::Online;
  in.linkUp = true;
  in.rssiDbm = rssiDbm;
  in.unitsIdle = unitsIdle;
  return in;
}

static WifiTxInput linkDown(bool unitsIdle = true) {
  WifiTxInput in;
  in.phase = WifiTxPhase::Online;
  in.linkUp = false;
  in.unitsIdle = unitsIdle;
  return in;
}

// Runs the policy once a second over [fromMs, toMs] and returns the largest
// single-tick change — the "never jump" instrument.
static int runSeconds(WifiTxState& st, const WifiTxInput& in, uint32_t fromMs,
                      uint32_t toMs) {
  int worst = 0;
  for (uint32_t t = fromMs; t <= toMs; t += 1000) {
    int before = st.index;
    int after = wifiTxPolicyStep(st, in, t);
    int delta = after > before ? after - before : before - after;
    if (delta > worst) worst = delta;
  }
  return worst;
}

// --- the level table ---------------------------------------------------------

static void test_table_is_the_idf_real_levels_ascending() {
  // esp_wifi_set_max_tx_power() rounds DOWN to these; anything else in the
  // table would make two "steps" the same level.
  static const int8_t idf[] = {8, 20, 28, 34, 44, 52, 56, 60, 66, 72, 80};
  TEST_ASSERT_EQUAL(sizeof(idf), WIFI_TX_LEVEL_COUNT);
  for (uint8_t i = 0; i < WIFI_TX_LEVEL_COUNT; i++) {
    TEST_ASSERT_EQUAL_INT8(idf[i], wifiTxLevelRaw(i));
  }
}

static void test_level_reports_tenths_of_dbm() {
  TEST_ASSERT_EQUAL(20, wifiTxLevelDbm10(0));
  TEST_ASSERT_EQUAL(85, wifiTxLevelDbm10(3));
  TEST_ASSERT_EQUAL(130, wifiTxLevelDbm10(5));
  TEST_ASSERT_EQUAL(165, wifiTxLevelDbm10(8));
}

static void test_out_of_range_index_clamps_to_the_top_level() {
  TEST_ASSERT_EQUAL_INT8(80, wifiTxLevelRaw(200));
}

// --- joining -----------------------------------------------------------------

static void test_boot_starts_at_the_lowest_level() {
  WifiTxState st;
  TEST_ASSERT_EQUAL(0, wifiTxPolicyStep(st, joining(), 1000));
}

static void test_join_holds_the_level_until_the_step_time() {
  WifiTxState st;
  wifiTxPolicyStep(st, joining(), 1000);
  TEST_ASSERT_EQUAL(
      0, wifiTxPolicyStep(st, joining(), 1000 + WIFI_TX_JOIN_STEP_MS - 1));
}

static void test_join_steps_one_level_after_the_step_time() {
  WifiTxState st;
  wifiTxPolicyStep(st, joining(), 1000);
  TEST_ASSERT_EQUAL(1,
                    wifiTxPolicyStep(st, joining(), 1000 + WIFI_TX_JOIN_STEP_MS));
}

static void test_join_never_passes_the_join_ceiling() {
  // An access point that is simply off must not ramp the board up during
  // boot, where the supply margin is thinnest.
  WifiTxState st;
  runSeconds(st, joining(), 0, 600000);
  TEST_ASSERT_EQUAL(WIFI_TX_JOIN_CEILING_INDEX, st.index);
}

static void test_join_reaches_its_ceiling_inside_the_join_window() {
  // The ladder must leave time to join at the ceiling before WifiPolicy's
  // 30 s window gives up (a join takes ~5 s on the bench).
  WifiTxState st;
  runSeconds(st, joining(), 0, 30000 - 6000);
  TEST_ASSERT_EQUAL(WIFI_TX_JOIN_CEILING_INDEX, st.index);
}

static void test_join_never_jumps() {
  WifiTxState st;
  TEST_ASSERT_EQUAL(1, runSeconds(st, joining(), 0, 600000));
}

// --- online: stepping up -------------------------------------------------------

static void test_online_keeps_the_level_it_joined_at() {
  WifiTxState st;
  runSeconds(st, joining(), 0, WIFI_TX_JOIN_STEP_MS);  // -> level 1
  TEST_ASSERT_EQUAL(1, wifiTxPolicyStep(st, online(STRONG_RSSI), 9000));
}

static void test_strong_link_stays_at_the_lowest_level() {
  WifiTxState st;
  wifiTxPolicyStep(st, joining(), 0);
  runSeconds(st, online(STRONG_RSSI), 1000, 3600000);
  TEST_ASSERT_EQUAL(0, st.index);
}

static void test_weak_signal_steps_up_only_after_the_weak_window() {
  WifiTxState st;
  wifiTxPolicyStep(st, online(WEAK_RSSI), 1000);
  TEST_ASSERT_EQUAL(
      0, wifiTxPolicyStep(st, online(WEAK_RSSI), 1000 + WIFI_TX_WEAK_MS - 1));
  TEST_ASSERT_EQUAL(
      1, wifiTxPolicyStep(st, online(WEAK_RSSI), 1000 + WIFI_TX_WEAK_MS));
}

static void test_a_good_sample_restarts_the_weak_window() {
  WifiTxState st;
  wifiTxPolicyStep(st, online(WEAK_RSSI), 1000);
  wifiTxPolicyStep(st, online(STRONG_RSSI), 30000);
  TEST_ASSERT_EQUAL(
      0, wifiTxPolicyStep(st, online(WEAK_RSSI), 1000 + WIFI_TX_WEAK_MS));
}

static void test_weak_threshold_follows_the_level() {
  // -64 dBm heard from the access point: too little to be heard back at
  // 2 dBm, enough at 5 dBm — so the ladder climbs exactly one level.
  WifiTxState st;
  runSeconds(st, online(-64), 0, 3 * WIFI_TX_WEAK_MS);
  TEST_ASSERT_EQUAL(1, st.index);
}

static void test_online_never_passes_the_online_ceiling() {
  WifiTxState st;
  runSeconds(st, online(WEAK_RSSI), 0, 3600000);
  TEST_ASSERT_EQUAL(WIFI_TX_ONLINE_CEILING_INDEX, st.index);
}

static void test_online_never_jumps() {
  WifiTxState st;
  TEST_ASSERT_EQUAL(1, runSeconds(st, online(WEAK_RSSI), 0, 3600000));
}

static void test_step_up_waits_for_idle_units() {
  WifiTxState st;
  runSeconds(st, online(WEAK_RSSI, false), 0, 10 * WIFI_TX_WEAK_MS);
  TEST_ASSERT_EQUAL(0, st.index);
  // The moment the units go idle the already-earned step lands.
  uint32_t t = 10 * WIFI_TX_WEAK_MS + 1000;
  TEST_ASSERT_EQUAL(1, wifiTxPolicyStep(st, online(WEAK_RSSI, true), t));
}

static void test_no_step_up_while_the_boot_settles() {
  // The first minute online is still the boot window (units homing, supply
  // margin thinnest): a drop there is remembered, not acted on.
  WifiTxState st;
  wifiTxPolicyStep(st, online(STRONG_RSSI), 1000);
  TEST_ASSERT_EQUAL(0, wifiTxPolicyStep(st, linkDown(), 2000));
  TEST_ASSERT_EQUAL(
      0, wifiTxPolicyStep(st, linkDown(), 1000 + WIFI_TX_SETTLE_MS - 1));
  TEST_ASSERT_EQUAL(1,
                    wifiTxPolicyStep(st, linkDown(), 1000 + WIFI_TX_SETTLE_MS));
}

// First tick online at 1000 ms; returns the first instant past the settle.
static uint32_t settle(WifiTxState& st) {
  wifiTxPolicyStep(st, online(STRONG_RSSI), 1000);
  return 1000 + WIFI_TX_SETTLE_MS;
}

static void test_a_link_drop_steps_up_one_level() {
  WifiTxState st;
  uint32_t t = settle(st);
  TEST_ASSERT_EQUAL(1, wifiTxPolicyStep(st, linkDown(), t));
}

static void test_a_long_outage_is_still_one_step() {
  // An access point reboot is one drop, however long it lasts.
  WifiTxState st;
  uint32_t t = settle(st);
  runSeconds(st, linkDown(), t, t + 90000);
  TEST_ASSERT_EQUAL(1, st.index);
}

static void test_a_link_drop_step_waits_for_idle_units() {
  WifiTxState st;
  uint32_t t = settle(st);
  TEST_ASSERT_EQUAL(0, wifiTxPolicyStep(st, linkDown(false), t));
  TEST_ASSERT_EQUAL(1, wifiTxPolicyStep(st, linkDown(true), t + 1000));
}

static void test_flapping_link_is_rate_limited_by_the_up_dwell() {
  WifiTxState st;
  uint32_t t = settle(st);
  wifiTxPolicyStep(st, linkDown(), t);  // -> 1
  wifiTxPolicyStep(st, online(STRONG_RSSI), t + 1000);
  TEST_ASSERT_EQUAL(1, wifiTxPolicyStep(st, linkDown(), t + 2000));
  wifiTxPolicyStep(st, online(STRONG_RSSI), t + 3000);
  TEST_ASSERT_EQUAL(
      2, wifiTxPolicyStep(st, linkDown(), t + WIFI_TX_UP_DWELL_MS));
}

static void test_a_drop_that_recovers_in_the_settle_still_earns_its_step() {
  // A drop is evidence whether or not the link came back by itself.
  WifiTxState st;
  uint32_t t = settle(st);
  wifiTxPolicyStep(st, linkDown(), 5000);
  wifiTxPolicyStep(st, online(STRONG_RSSI), 6000);
  TEST_ASSERT_EQUAL(1, wifiTxPolicyStep(st, online(STRONG_RSSI), t));
}

// --- online: stepping down -----------------------------------------------------

static void test_spare_signal_steps_down_only_after_the_healthy_window() {
  WifiTxState st;
  st.index = 4;
  wifiTxPolicyStep(st, online(STRONG_RSSI), 1000);
  TEST_ASSERT_EQUAL(4, wifiTxPolicyStep(st, online(STRONG_RSSI),
                                        1000 + WIFI_TX_HEALTHY_MS - 1));
  TEST_ASSERT_EQUAL(
      3, wifiTxPolicyStep(st, online(STRONG_RSSI), 1000 + WIFI_TX_HEALTHY_MS));
}

static void test_step_down_never_jumps_and_ends_at_the_lowest_level() {
  WifiTxState st;
  st.index = WIFI_TX_ONLINE_CEILING_INDEX;
  TEST_ASSERT_EQUAL(1, runSeconds(st, online(STRONG_RSSI), 0,
                                  10 * WIFI_TX_HEALTHY_MS));
  TEST_ASSERT_EQUAL(0, st.index);
}

static void test_a_link_drop_restarts_the_healthy_window() {
  WifiTxState st;
  st.index = 4;
  wifiTxPolicyStep(st, online(STRONG_RSSI), 1000);
  wifiTxPolicyStep(st, linkDown(), 300000);  // -> 5
  TEST_ASSERT_EQUAL(5, wifiTxPolicyStep(st, online(STRONG_RSSI),
                                        1000 + WIFI_TX_HEALTHY_MS));
}

static void test_a_drop_at_the_ceiling_does_not_pin_the_level_there() {
  // Nothing higher to step to: the drop is spent, and the level must still
  // come back down once the link is healthy.
  WifiTxState st;
  st.index = WIFI_TX_ONLINE_CEILING_INDEX;
  uint32_t t = settle(st);
  wifiTxPolicyStep(st, linkDown(), t);
  TEST_ASSERT_EQUAL(WIFI_TX_ONLINE_CEILING_INDEX, st.index);
  runSeconds(st, online(STRONG_RSSI), t + 1000,
             t + 1000 + WIFI_TX_HEALTHY_MS + 1000);
  TEST_ASSERT_EQUAL(WIFI_TX_ONLINE_CEILING_INDEX - 1, st.index);
}

static void test_no_step_down_onto_a_level_that_would_be_weak() {
  // Hysteresis: after climbing for a signal, the same signal must not bring
  // the level straight back down.
  WifiTxState st;
  runSeconds(st, online(-64), 0, 3 * WIFI_TX_WEAK_MS);  // settles at 1
  runSeconds(st, online(-64), 3 * WIFI_TX_WEAK_MS + 1000,
             5 * WIFI_TX_HEALTHY_MS);
  TEST_ASSERT_EQUAL(1, st.index);
  TEST_ASSERT_EQUAL(0, st.stepsDown);
}

// --- portal + bookkeeping ------------------------------------------------------

static void test_portal_runs_at_the_fixed_portal_level() {
  WifiTxState st;
  WifiTxInput in;
  in.phase = WifiTxPhase::Portal;
  TEST_ASSERT_EQUAL(WIFI_TX_PORTAL_INDEX, wifiTxPolicyStep(st, in, 1000));
  TEST_ASSERT_EQUAL(WIFI_TX_PORTAL_INDEX, wifiTxPolicyStep(st, in, 900000));
}

static void test_steps_are_counted() {
  WifiTxState st;
  runSeconds(st, online(WEAK_RSSI), 0, 3600000);
  TEST_ASSERT_EQUAL(WIFI_TX_ONLINE_CEILING_INDEX, st.stepsUp);
  runSeconds(st, online(STRONG_RSSI), 3601000, 3601000 + 10 * WIFI_TX_HEALTHY_MS);
  TEST_ASSERT_EQUAL(WIFI_TX_ONLINE_CEILING_INDEX, st.stepsDown);
}

static void test_survives_millis_rollover() {
  WifiTxState st;
  uint32_t t0 = 0xFFFFFFFFUL - 20000;
  wifiTxPolicyStep(st, online(WEAK_RSSI), t0);
  TEST_ASSERT_EQUAL(
      0, wifiTxPolicyStep(st, online(WEAK_RSSI), t0 + WIFI_TX_WEAK_MS - 1));
  TEST_ASSERT_EQUAL(
      1, wifiTxPolicyStep(st, online(WEAK_RSSI), t0 + WIFI_TX_WEAK_MS));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_table_is_the_idf_real_levels_ascending);
  RUN_TEST(test_level_reports_tenths_of_dbm);
  RUN_TEST(test_out_of_range_index_clamps_to_the_top_level);
  RUN_TEST(test_boot_starts_at_the_lowest_level);
  RUN_TEST(test_join_holds_the_level_until_the_step_time);
  RUN_TEST(test_join_steps_one_level_after_the_step_time);
  RUN_TEST(test_join_never_passes_the_join_ceiling);
  RUN_TEST(test_join_reaches_its_ceiling_inside_the_join_window);
  RUN_TEST(test_join_never_jumps);
  RUN_TEST(test_online_keeps_the_level_it_joined_at);
  RUN_TEST(test_strong_link_stays_at_the_lowest_level);
  RUN_TEST(test_weak_signal_steps_up_only_after_the_weak_window);
  RUN_TEST(test_a_good_sample_restarts_the_weak_window);
  RUN_TEST(test_weak_threshold_follows_the_level);
  RUN_TEST(test_online_never_passes_the_online_ceiling);
  RUN_TEST(test_online_never_jumps);
  RUN_TEST(test_step_up_waits_for_idle_units);
  RUN_TEST(test_no_step_up_while_the_boot_settles);
  RUN_TEST(test_a_link_drop_steps_up_one_level);
  RUN_TEST(test_a_long_outage_is_still_one_step);
  RUN_TEST(test_a_link_drop_step_waits_for_idle_units);
  RUN_TEST(test_flapping_link_is_rate_limited_by_the_up_dwell);
  RUN_TEST(test_spare_signal_steps_down_only_after_the_healthy_window);
  RUN_TEST(test_step_down_never_jumps_and_ends_at_the_lowest_level);
  RUN_TEST(test_a_link_drop_restarts_the_healthy_window);
  RUN_TEST(test_a_drop_that_recovers_in_the_settle_still_earns_its_step);
  RUN_TEST(test_a_drop_at_the_ceiling_does_not_pin_the_level_there);
  RUN_TEST(test_no_step_down_onto_a_level_that_would_be_weak);
  RUN_TEST(test_portal_runs_at_the_fixed_portal_level);
  RUN_TEST(test_steps_are_counted);
  RUN_TEST(test_survives_millis_rollover);
  return UNITY_END();
}
