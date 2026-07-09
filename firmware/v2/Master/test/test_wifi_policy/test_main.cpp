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

static void test_join_timeout_opens_portal() {
  WifiPolicyState st;
  quietStep(st, 1000, true);
  TEST_ASSERT_EQUAL(WifiAction::StartPortal,
                    quietStep(st, 1000 + WIFI_JOIN_TIMEOUT_MS, true));
  TEST_ASSERT_EQUAL(WifiPhase::Portal, st.phase);
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

static void test_link_drop_after_connect_never_reopens_portal() {
  // v1 parity: WiFi.setAutoReconnect(true) owns reconnection; the policy
  // stays parked in Connected no matter what the link reports.
  WifiPolicyState st;
  quietStep(st, 1000, true);
  wifiPolicyStep(st, 2000, true, true, false);  // -> Connected
  TEST_ASSERT_EQUAL(WifiAction::None, quietStep(st, 500000, true));
  TEST_ASSERT_EQUAL(WifiAction::None,
                    quietStep(st, 500000 + WIFI_PORTAL_TIMEOUT_MS, true));
  TEST_ASSERT_EQUAL(WifiPhase::Connected, st.phase);
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
  TEST_ASSERT_EQUAL(WifiAction::StartPortal, quietStep(st, afterWrap, true));
}

// ---------------------------------------------------------------------------

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_boot_with_creds_starts_join);
  RUN_TEST(test_boot_without_creds_goes_straight_to_portal);
  RUN_TEST(test_join_success_goes_online);
  RUN_TEST(test_join_window_still_open_just_before_timeout);
  RUN_TEST(test_join_timeout_opens_portal);
  RUN_TEST(test_link_up_wins_over_simultaneous_timeout);
  RUN_TEST(test_portal_submission_saves_and_reboots);
  RUN_TEST(test_portal_window_still_open_just_before_timeout);
  RUN_TEST(test_portal_timeout_reboots_to_retry);
  RUN_TEST(test_portal_submission_wins_over_simultaneous_timeout);
  RUN_TEST(test_config_submitted_while_connected_saves_and_reboots);
  RUN_TEST(test_link_drop_after_connect_never_reopens_portal);
  RUN_TEST(test_join_timeout_survives_millis_rollover);
  return UNITY_END();
}
