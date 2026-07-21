// Host-side unit tests for RescueWifiPolicy.h (#195) — the trimmed
// join-or-AP decision machine behind the rescue app's network bring-up.
// Deliberately smaller than Master's WifiPolicy.h: no portal submission, no
// reboot-retry — a rescue image never reboots itself out from under a
// recovery in progress (an otadata-erased device would just boot rescue
// again anyway). #349: an idle AP with stored credentials retries the STA
// join periodically so the open flash-capable network doesn't outlive a
// transient LAN outage; an in-use AP (associated station) holds open.

#include <ArduinoFake.h>
#include <unity.h>

#include "../../RescueWifiPolicy.h"

void setUp() {}
void tearDown() {}

// Convenience: one step with no link and an idle AP.
static RescueAction quietStep(RescuePolicyState& st, uint32_t nowMs,
                              bool credsStored) {
  return rescuePolicyStep(st, nowMs, false, credsStored, 0);
}

// --- boot dispatch -----------------------------------------------------------

static void test_boot_with_creds_starts_join() {
  RescuePolicyState st;
  TEST_ASSERT_EQUAL(RescueAction::StartJoin, quietStep(st, 1000, true));
  TEST_ASSERT_EQUAL(RescuePhase::Joining, st.phase);
}

static void test_boot_without_creds_goes_straight_to_ap() {
  // No credentials in NVS (or namespace missing): no 30 s wait for a join
  // that cannot happen — same rationale as Master's portal dispatch.
  RescuePolicyState st;
  TEST_ASSERT_EQUAL(RescueAction::StartAp, quietStep(st, 1000, false));
  TEST_ASSERT_EQUAL(RescuePhase::Ap, st.phase);
}

// --- joining ------------------------------------------------------------------

static void test_join_success_goes_online() {
  RescuePolicyState st;
  quietStep(st, 1000, true);
  TEST_ASSERT_EQUAL(RescueAction::None, quietStep(st, 5000, true));
  TEST_ASSERT_EQUAL(RescueAction::StartOnline,
                    rescuePolicyStep(st, 12000, true, true, 0));
  TEST_ASSERT_EQUAL(RescuePhase::Online, st.phase);
}

static void test_join_window_still_open_just_before_timeout() {
  RescuePolicyState st;
  quietStep(st, 1000, true);
  TEST_ASSERT_EQUAL(RescueAction::None,
                    quietStep(st, 1000 + RESCUE_JOIN_TIMEOUT_MS - 1, true));
  TEST_ASSERT_EQUAL(RescuePhase::Joining, st.phase);
}

static void test_join_timeout_opens_ap() {
  RescuePolicyState st;
  quietStep(st, 1000, true);
  TEST_ASSERT_EQUAL(RescueAction::StartAp,
                    quietStep(st, 1000 + RESCUE_JOIN_TIMEOUT_MS, true));
  TEST_ASSERT_EQUAL(RescuePhase::Ap, st.phase);
}

static void test_link_up_at_deadline_wins_over_timeout() {
  // Connected exactly at the deadline must go online, not throw away a live
  // association for an AP (same ordering rule as Master's policy).
  RescuePolicyState st;
  quietStep(st, 1000, true);
  TEST_ASSERT_EQUAL(
      RescueAction::StartOnline,
      rescuePolicyStep(st, 1000 + RESCUE_JOIN_TIMEOUT_MS, true, true, 0));
  TEST_ASSERT_EQUAL(RescuePhase::Online, st.phase);
}

// --- terminal phases ----------------------------------------------------------

static void test_ap_without_creds_stays_up_forever() {
  // No stored credentials: the AP is the only recovery path — never retire
  // it (and never reboot; rescue must not cycle out from under a recovery).
  RescuePolicyState st;
  quietStep(st, 1000, false);
  TEST_ASSERT_EQUAL(RescuePhase::Ap, st.phase);
  TEST_ASSERT_EQUAL(RescueAction::None, quietStep(st, 1000 + 3600000UL, false));
  TEST_ASSERT_EQUAL(RescueAction::None, quietStep(st, 1000 + 7200000UL, false));
  TEST_ASSERT_EQUAL(RescuePhase::Ap, st.phase);
}

static void test_ap_with_creds_idle_retries_join_after_window() {
  // #349: an idle AP with stored credentials must not broadcast forever —
  // after the retry window it re-attempts the STA join.
  RescuePolicyState st;
  quietStep(st, 1000, true);                        // Boot -> Joining
  quietStep(st, 1000 + RESCUE_JOIN_TIMEOUT_MS, true);  // join timeout -> Ap
  TEST_ASSERT_EQUAL(RescuePhase::Ap, st.phase);
  const uint32_t apStart = 1000 + RESCUE_JOIN_TIMEOUT_MS;
  TEST_ASSERT_EQUAL(RescueAction::None,
                    quietStep(st, apStart + RESCUE_AP_RETRY_MS - 1, true));
  TEST_ASSERT_EQUAL(RescueAction::StartJoin,
                    quietStep(st, apStart + RESCUE_AP_RETRY_MS, true));
  TEST_ASSERT_EQUAL(RescuePhase::Joining, st.phase);
}

static void test_ap_retry_join_failure_returns_to_ap() {
  RescuePolicyState st;
  quietStep(st, 1000, true);
  quietStep(st, 1000 + RESCUE_JOIN_TIMEOUT_MS, true);  // -> Ap
  const uint32_t retryAt = 1000 + RESCUE_JOIN_TIMEOUT_MS + RESCUE_AP_RETRY_MS;
  TEST_ASSERT_EQUAL(RescueAction::StartJoin, quietStep(st, retryAt, true));
  // Retry join times out too: back to a fresh AP with a fresh retry window.
  TEST_ASSERT_EQUAL(RescueAction::StartAp,
                    quietStep(st, retryAt + RESCUE_JOIN_TIMEOUT_MS, true));
  TEST_ASSERT_EQUAL(RescuePhase::Ap, st.phase);
  TEST_ASSERT_EQUAL(RescueAction::None,
                    quietStep(st, retryAt + RESCUE_JOIN_TIMEOUT_MS +
                                      RESCUE_AP_RETRY_MS - 1,
                              true));
}

static void test_ap_retry_join_success_goes_online() {
  RescuePolicyState st;
  quietStep(st, 1000, true);
  quietStep(st, 1000 + RESCUE_JOIN_TIMEOUT_MS, true);  // -> Ap
  const uint32_t retryAt = 1000 + RESCUE_JOIN_TIMEOUT_MS + RESCUE_AP_RETRY_MS;
  TEST_ASSERT_EQUAL(RescueAction::StartJoin, quietStep(st, retryAt, true));
  TEST_ASSERT_EQUAL(RescueAction::StartOnline,
                    rescuePolicyStep(st, retryAt + 4000, true, true, 0));
  TEST_ASSERT_EQUAL(RescuePhase::Online, st.phase);
}

static void test_ap_in_use_holds_open_past_retry_window() {
  // A recovery in progress (associated station) must never be interrupted:
  // the retry deadline keeps sliding while anyone is on the AP, and re-arms
  // a full window from the moment the last client leaves.
  RescuePolicyState st;
  quietStep(st, 1000, true);
  quietStep(st, 1000 + RESCUE_JOIN_TIMEOUT_MS, true);  // -> Ap
  const uint32_t apStart = 1000 + RESCUE_JOIN_TIMEOUT_MS;
  const uint32_t busyUntil = apStart + RESCUE_AP_RETRY_MS + 600000UL;
  TEST_ASSERT_EQUAL(RescueAction::None,
                    rescuePolicyStep(st, busyUntil, false, true, 1));
  TEST_ASSERT_EQUAL(RescuePhase::Ap, st.phase);
  // Client gone: still a full retry window before the next join attempt.
  TEST_ASSERT_EQUAL(RescueAction::None,
                    quietStep(st, busyUntil + RESCUE_AP_RETRY_MS - 1, true));
  TEST_ASSERT_EQUAL(RescueAction::StartJoin,
                    quietStep(st, busyUntil + RESCUE_AP_RETRY_MS, true));
}

static void test_online_link_drop_stays_online() {
  // Link drops belong to the SDK's auto-reconnect, never a mode change.
  RescuePolicyState st;
  quietStep(st, 1000, true);
  rescuePolicyStep(st, 2000, true, true, 0);  // -> Online
  TEST_ASSERT_EQUAL(RescueAction::None, quietStep(st, 60000, true));
  TEST_ASSERT_EQUAL(RescuePhase::Online, st.phase);
}

// --- rollover safety ----------------------------------------------------------

static void test_join_deadline_is_rollover_safe() {
  // Boot near the uint32 millis wrap: deadline arithmetic must still fire.
  const uint32_t nearWrap = 0xFFFFFFFFUL - 5000UL;
  RescuePolicyState st;
  TEST_ASSERT_EQUAL(RescueAction::StartJoin, quietStep(st, nearWrap, true));
  // 1 ms before the (wrapped) deadline: window still open.
  const uint32_t wrapped = nearWrap + RESCUE_JOIN_TIMEOUT_MS;  // wraps
  TEST_ASSERT_EQUAL(RescueAction::None, quietStep(st, wrapped - 1, true));
  TEST_ASSERT_EQUAL(RescueAction::StartAp, quietStep(st, wrapped, true));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_boot_with_creds_starts_join);
  RUN_TEST(test_boot_without_creds_goes_straight_to_ap);
  RUN_TEST(test_join_success_goes_online);
  RUN_TEST(test_join_window_still_open_just_before_timeout);
  RUN_TEST(test_join_timeout_opens_ap);
  RUN_TEST(test_link_up_at_deadline_wins_over_timeout);
  RUN_TEST(test_ap_without_creds_stays_up_forever);
  RUN_TEST(test_ap_with_creds_idle_retries_join_after_window);
  RUN_TEST(test_ap_retry_join_failure_returns_to_ap);
  RUN_TEST(test_ap_retry_join_success_goes_online);
  RUN_TEST(test_ap_in_use_holds_open_past_retry_window);
  RUN_TEST(test_online_link_drop_stays_online);
  RUN_TEST(test_join_deadline_is_rollover_safe);
  return UNITY_END();
}
