// Host-side unit tests for WifiPolicy.h (#188) — the pure join/portal
// supervision state machine behind netTask's WiFi bring-up. v1 parity
// (ServiceWifiFunctions.ino initWiFi()): 30 s bounded join on stored
// credentials, then a 300 s "<name>-setup" portal, then a reboot-retry
// cycle; once connected, link drops belong to the SDK's auto-reconnect and
// never re-open the portal.

#include <ArduinoFake.h>
#include <unity.h>

#include "../../WifiPolicy.h"

void setUp() {}
void tearDown() {}

// Convenience: run one step with no link, no portal submission.
static WifiAction quietStep(WifiPolicyState& st, uint32_t nowMs,
                            bool credsStored) {
  return wifiPolicyStep(st, nowMs, false, credsStored, false);
}

// --- boot dispatch -----------------------------------------------------------

static void test_boot_with_creds_starts_join() {
  WifiPolicyState st;
  TEST_ASSERT_EQUAL(WifiAction::StartJoin, quietStep(st, 1000, true));
  TEST_ASSERT_EQUAL(WifiPhase::Joining, st.phase);
}

static void test_boot_without_creds_goes_straight_to_portal() {
  // v1: tryJoinKnownWifi() returns immediately false with no stored
  // credentials — no 30 s wait for a join that cannot happen.
  WifiPolicyState st;
  TEST_ASSERT_EQUAL(WifiAction::StartPortal, quietStep(st, 1000, false));
  TEST_ASSERT_EQUAL(WifiPhase::Portal, st.phase);
}

// --- joining ------------------------------------------------------------------

static void test_join_success_goes_online() {
  WifiPolicyState st;
  quietStep(st, 1000, true);
  TEST_ASSERT_EQUAL(WifiAction::None, quietStep(st, 5000, true));
  TEST_ASSERT_EQUAL(WifiAction::StartOnline,
                    wifiPolicyStep(st, 12000, true, true, false));
  TEST_ASSERT_EQUAL(WifiPhase::Connected, st.phase);
}

static void test_join_window_still_open_just_before_timeout() {
  WifiPolicyState st;
  quietStep(st, 1000, true);
  TEST_ASSERT_EQUAL(WifiAction::None,
                    quietStep(st, 1000 + WIFI_JOIN_TIMEOUT_MS - 1, true));
  TEST_ASSERT_EQUAL(WifiPhase::Joining, st.phase);
}

// One failed association must not park the board in the portal for five
// minutes (#524): the first windows that close ask for a restart instead.
static void test_first_join_timeout_asks_for_a_retry_not_the_portal() {
  WifiPolicyState st;
  quietStep(st, 1000, true);
  TEST_ASSERT_EQUAL(WifiAction::RetryJoin,
                    quietStep(st, 1000 + WIFI_JOIN_TIMEOUT_MS, true));
  TEST_ASSERT_EQUAL(WifiPhase::Joining, st.phase);
  // Still the answer on a later step: the caller restarts, the policy does
  // not fall through to the portal on its own.
  TEST_ASSERT_EQUAL(WifiAction::RetryJoin,
                    quietStep(st, 2000 + WIFI_JOIN_TIMEOUT_MS, true));
}

static void test_every_attempt_before_the_last_retries() {
  for (uint8_t failed = 0; failed + 1 < WIFI_JOIN_ATTEMPTS; failed++) {
    WifiPolicyState st;
    st.failedJoinBoots = failed;
    quietStep(st, 1000, true);
    TEST_ASSERT_EQUAL(WifiAction::RetryJoin,
                      quietStep(st, 1000 + WIFI_JOIN_TIMEOUT_MS, true));
  }
}

static void test_last_attempt_timeout_opens_portal() {
  WifiPolicyState st;
  st.failedJoinBoots = WIFI_JOIN_ATTEMPTS - 1;
  quietStep(st, 1000, true);
  TEST_ASSERT_EQUAL(WifiAction::None,
                    quietStep(st, 1000 + WIFI_JOIN_TIMEOUT_MS - 1, true));
  TEST_ASSERT_EQUAL(WifiAction::StartPortal,
                    quietStep(st, 1000 + WIFI_JOIN_TIMEOUT_MS, true));
  TEST_ASSERT_EQUAL(WifiPhase::Portal, st.phase);
}

// Three windows is the promise; a different number changes how long a board
// with a dead network stays away from its portal.
static void test_three_join_attempts_before_the_portal() {
  TEST_ASSERT_EQUAL_UINT8(3, WIFI_JOIN_ATTEMPTS);
}

static void test_a_retry_boot_that_joins_goes_online() {
  WifiPolicyState st;
  st.failedJoinBoots = 1;
  quietStep(st, 1000, true);
  TEST_ASSERT_EQUAL(WifiAction::StartOnline,
                    wifiPolicyStep(st, 4000, true, true, false));
}

static void test_no_credentials_never_retry() {
  WifiPolicyState st;
  st.failedJoinBoots = 1;  // stale tally: credentials were erased since
  TEST_ASSERT_EQUAL(WifiAction::StartPortal, quietStep(st, 1000, false));
}

// --- restarts wait for a unit reflash -----------------------------------------

static void test_a_join_retry_waits_for_a_unit_reflash() {
  TEST_ASSERT_FALSE(wifiRestartMayProceed(WifiAction::RetryJoin,
                                          WifiPhase::Joining, true));
  TEST_ASSERT_TRUE(wifiRestartMayProceed(WifiAction::RetryJoin,
                                         WifiPhase::Joining, false));
}

static void test_the_portal_timeout_restart_waits_for_a_unit_reflash() {
  TEST_ASSERT_FALSE(wifiRestartMayProceed(WifiAction::Reboot,
                                          WifiPhase::Portal, true));
  TEST_ASSERT_TRUE(wifiRestartMayProceed(WifiAction::Reboot,
                                         WifiPhase::Portal, false));
}

// The link-loss watchdog is the only way back onto the network; a reflash
// must not be able to hold it off.
static void test_the_link_loss_restart_never_waits() {
  TEST_ASSERT_TRUE(wifiRestartMayProceed(WifiAction::Reboot,
                                         WifiPhase::Connected, true));
}

static void test_a_saved_configuration_restart_never_waits() {
  TEST_ASSERT_TRUE(wifiRestartMayProceed(WifiAction::SaveAndReboot,
                                         WifiPhase::Portal, true));
}

// --- the tally between boots -------------------------------------------------

static void test_retry_record_roundtrip() {
  WifiJoinRetryRecord r;
  for (uint8_t n = 0; n < WIFI_JOIN_ATTEMPTS; n++) {
    wifiJoinRetryEncode(r, n);
    TEST_ASSERT_EQUAL_UINT8(n, wifiJoinRetryDecode(r));
  }
}

// After power-on the record is whatever the RAM held.
static void test_retry_record_garbage_reads_as_no_failures() {
  WifiJoinRetryRecord zeros = {0, 0, 0};
  WifiJoinRetryRecord ones = {0xFFFFFFFFUL, 0xFF, 0xFF};
  WifiJoinRetryRecord magicOnly = {WIFI_JOIN_RETRY_MAGIC, 2, 2};
  TEST_ASSERT_EQUAL_UINT8(0, wifiJoinRetryDecode(zeros));
  TEST_ASSERT_EQUAL_UINT8(0, wifiJoinRetryDecode(ones));
  TEST_ASSERT_EQUAL_UINT8(0, wifiJoinRetryDecode(magicOnly));
}

static void test_retry_record_never_leaves_a_boot_without_an_attempt() {
  WifiJoinRetryRecord r;
  wifiJoinRetryEncode(r, 200);
  uint8_t failed = wifiJoinRetryDecode(r);
  TEST_ASSERT_TRUE(failed < WIFI_JOIN_ATTEMPTS);
  // That boot still spends its window before the portal.
  WifiPolicyState st;
  st.failedJoinBoots = failed;
  TEST_ASSERT_EQUAL(WifiAction::StartJoin, quietStep(st, 1000, true));
}

static void test_link_up_wins_over_simultaneous_timeout() {
  // Same tick carries both "connected" and "window expired": connecting
  // must win — the portal would throw away a live association.
  WifiPolicyState st;
  quietStep(st, 1000, true);
  TEST_ASSERT_EQUAL(WifiAction::StartOnline,
                    wifiPolicyStep(st, 1000 + WIFI_JOIN_TIMEOUT_MS, true, true,
                                   false));
  TEST_ASSERT_EQUAL(WifiPhase::Connected, st.phase);
}

// --- portal --------------------------------------------------------------------

static void test_portal_submission_saves_and_reboots() {
  WifiPolicyState st;
  quietStep(st, 1000, false);
  TEST_ASSERT_EQUAL(WifiAction::None, quietStep(st, 2000, false));
  TEST_ASSERT_EQUAL(WifiAction::SaveAndReboot,
                    wifiPolicyStep(st, 60000, false, false, true));
}

static void test_portal_window_still_open_just_before_timeout() {
  WifiPolicyState st;
  quietStep(st, 1000, false);
  TEST_ASSERT_EQUAL(WifiAction::None,
                    quietStep(st, 1000 + WIFI_PORTAL_TIMEOUT_MS - 1, false));
  TEST_ASSERT_EQUAL(WifiPhase::Portal, st.phase);
}

static void test_portal_timeout_reboots_to_retry() {
  // v1: nobody configured us inside the window — reboot and retry the
  // stored credentials (the router may just have been down).
  WifiPolicyState st;
  quietStep(st, 1000, false);
  TEST_ASSERT_EQUAL(WifiAction::Reboot,
                    quietStep(st, 1000 + WIFI_PORTAL_TIMEOUT_MS, false));
}

static void test_portal_submission_wins_over_simultaneous_timeout() {
  // A user hitting save in the portal's dying tick must not lose the
  // credentials to a plain retry reboot.
  WifiPolicyState st;
  quietStep(st, 1000, false);
  TEST_ASSERT_EQUAL(WifiAction::SaveAndReboot,
                    wifiPolicyStep(st, 1000 + WIFI_PORTAL_TIMEOUT_MS, false,
                                   false, true));
}

// --- connected is terminal ------------------------------------------------------

static void test_config_submitted_while_connected_saves_and_reboots() {
  // The portal page is reachable from the LAN too ("move the display to
  // another network"): a validated /wifi/config submission in Connected
  // must persist + reboot exactly like a portal one — not rot in staging.
  WifiPolicyState st;
  quietStep(st, 1000, true);
  wifiPolicyStep(st, 2000, true, true, false);  // -> Connected
  TEST_ASSERT_EQUAL(WifiAction::SaveAndReboot,
                    wifiPolicyStep(st, 90000, true, true, true));
}

// Convenience: reach Connected phase with a link, ready for drop tests.
static void connect(WifiPolicyState& st) {
  quietStep(st, 1000, true);
  wifiPolicyStep(st, 2000, true, true, false);  // -> Connected
}

static void test_brief_link_drop_after_connect_does_not_reboot() {
  // The SDK auto-reconnect owns transient drops: within the reconnect window
  // the policy stays parked in Connected, no reboot.
  WifiPolicyState st;
  connect(st);
  TEST_ASSERT_EQUAL(WifiAction::None,
                    wifiPolicyStep(st, 2000 + WIFI_RECONNECT_TIMEOUT_MS - 1,
                                   false, true, false));
  TEST_ASSERT_EQUAL(WifiPhase::Connected, st.phase);
}

static void test_sustained_link_drop_after_connect_reboots() {
  // #328: a continuous outage past the reconnect window means the SDK is
  // wedged — reboot to re-run the join. Never re-open the portal.
  WifiPolicyState st;
  connect(st);
  // First down tick arms the outage clock (link went down at t=2000).
  TEST_ASSERT_EQUAL(WifiAction::None, quietStep(st, 3000, true));
  WifiAction a =
      wifiPolicyStep(st, 3000 + WIFI_RECONNECT_TIMEOUT_MS, false, true, false);
  TEST_ASSERT_EQUAL(WifiAction::Reboot, a);
  TEST_ASSERT_EQUAL(WifiPhase::Connected, st.phase);  // reboot, not portal
}

static void test_link_recovery_resets_reconnect_watchdog() {
  // A drop that recovers before the window must not carry over: the outage
  // clock resets on link-up, so a later brief drop starts fresh.
  WifiPolicyState st;
  connect(st);
  quietStep(st, 3000, true);                                  // down, arm clock
  TEST_ASSERT_EQUAL(WifiAction::None,
                    wifiPolicyStep(st, 60000, true, true, false));  // recovered
  // A fresh drop 89 s later is still within a new window — no reboot.
  quietStep(st, 61000, true);
  TEST_ASSERT_EQUAL(WifiAction::None,
                    wifiPolicyStep(st, 61000 + WIFI_RECONNECT_TIMEOUT_MS - 1,
                                   false, true, false));
  TEST_ASSERT_EQUAL(WifiPhase::Connected, st.phase);
}

static void test_reconnect_watchdog_survives_millis_rollover() {
  // Outage clock armed just before the uint32 wrap; the deadline lands past
  // it. Signed-difference math must hold the window across the seam.
  WifiPolicyState st;
  connect(st);
  const uint32_t nearWrap = 0xFFFFFF00u;
  TEST_ASSERT_EQUAL(WifiAction::None, quietStep(st, nearWrap, true));  // arm
  const uint32_t afterWrap = nearWrap + WIFI_RECONNECT_TIMEOUT_MS;     // wrapped
  TEST_ASSERT_TRUE(afterWrap < nearWrap);                             // sanity
  TEST_ASSERT_EQUAL(WifiAction::None, quietStep(st, afterWrap - 1, true));
  TEST_ASSERT_EQUAL(WifiAction::Reboot, quietStep(st, afterWrap, true));
}

static void test_config_submitted_while_disconnected_still_saves_and_reboots() {
  // A "move to another network" submission must win even mid-outage — the
  // reconnect watchdog must not shadow a staged credential change.
  WifiPolicyState st;
  connect(st);
  quietStep(st, 3000, true);  // link down, watchdog armed
  TEST_ASSERT_EQUAL(WifiAction::SaveAndReboot,
                    wifiPolicyStep(st, 4000, false, true, true));
}

// --- millis() rollover -----------------------------------------------------------

static void test_join_timeout_survives_millis_rollover() {
  // Deadline lands past the uint32 wrap: 30 s after 0xFFFFFF00 wraps to a
  // numerically SMALLER value; naive `now >= deadline` compares would fire
  // instantly (or never). Signed-difference math must hold the window open
  // across the seam and close it on time.
  WifiPolicyState st;
  const uint32_t nearWrap = 0xFFFFFF00u;
  TEST_ASSERT_EQUAL(WifiAction::StartJoin, quietStep(st, nearWrap, true));
  TEST_ASSERT_EQUAL(WifiAction::None, quietStep(st, nearWrap + 1000, true));
  const uint32_t afterWrap = nearWrap + WIFI_JOIN_TIMEOUT_MS;  // wrapped
  TEST_ASSERT_TRUE(afterWrap < nearWrap);                      // sanity
  TEST_ASSERT_EQUAL(WifiAction::None, quietStep(st, afterWrap - 1, true));
  TEST_ASSERT_EQUAL(WifiAction::RetryJoin, quietStep(st, afterWrap, true));
}

// ---------------------------------------------------------------------------

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_boot_with_creds_starts_join);
  RUN_TEST(test_boot_without_creds_goes_straight_to_portal);
  RUN_TEST(test_join_success_goes_online);
  RUN_TEST(test_join_window_still_open_just_before_timeout);
  RUN_TEST(test_first_join_timeout_asks_for_a_retry_not_the_portal);
  RUN_TEST(test_every_attempt_before_the_last_retries);
  RUN_TEST(test_last_attempt_timeout_opens_portal);
  RUN_TEST(test_three_join_attempts_before_the_portal);
  RUN_TEST(test_a_retry_boot_that_joins_goes_online);
  RUN_TEST(test_no_credentials_never_retry);
  RUN_TEST(test_a_join_retry_waits_for_a_unit_reflash);
  RUN_TEST(test_the_portal_timeout_restart_waits_for_a_unit_reflash);
  RUN_TEST(test_the_link_loss_restart_never_waits);
  RUN_TEST(test_a_saved_configuration_restart_never_waits);
  RUN_TEST(test_retry_record_roundtrip);
  RUN_TEST(test_retry_record_garbage_reads_as_no_failures);
  RUN_TEST(test_retry_record_never_leaves_a_boot_without_an_attempt);
  RUN_TEST(test_link_up_wins_over_simultaneous_timeout);
  RUN_TEST(test_portal_submission_saves_and_reboots);
  RUN_TEST(test_portal_window_still_open_just_before_timeout);
  RUN_TEST(test_portal_timeout_reboots_to_retry);
  RUN_TEST(test_portal_submission_wins_over_simultaneous_timeout);
  RUN_TEST(test_config_submitted_while_connected_saves_and_reboots);
  RUN_TEST(test_brief_link_drop_after_connect_does_not_reboot);
  RUN_TEST(test_sustained_link_drop_after_connect_reboots);
  RUN_TEST(test_link_recovery_resets_reconnect_watchdog);
  RUN_TEST(test_reconnect_watchdog_survives_millis_rollover);
  RUN_TEST(test_config_submitted_while_disconnected_still_saves_and_reboots);
  RUN_TEST(test_join_timeout_survives_millis_rollover);
  return UNITY_END();
}
