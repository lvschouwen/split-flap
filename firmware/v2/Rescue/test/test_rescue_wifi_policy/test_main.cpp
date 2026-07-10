// Host-side unit tests for RescueWifiPolicy.h (#195) — the trimmed
// join-or-AP decision machine behind the rescue app's network bring-up.
// Deliberately smaller than Master's WifiPolicy.h: no portal submission, no
// portal timeout, no reboot-retry — a rescue image parks on its AP forever
// rather than reboot-cycling out from under a recovery in progress (an
// otadata-erased device would just boot rescue again anyway).

#include <ArduinoFake.h>
#include <unity.h>

#include "../../RescueWifiPolicy.h"

void setUp() {}
void tearDown() {}

// Convenience: one step with no link.
static RescueAction quietStep(RescuePolicyState& st, uint32_t nowMs,
                              bool credsStored) {
  return rescuePolicyStep(st, nowMs, false, credsStored);
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
                    rescuePolicyStep(st, 12000, true, true));
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
      rescuePolicyStep(st, 1000 + RESCUE_JOIN_TIMEOUT_MS, true, true));
  TEST_ASSERT_EQUAL(RescuePhase::Online, st.phase);
}

// --- terminal phases ----------------------------------------------------------

static void test_ap_is_terminal_no_timeout_no_reboot() {
  RescuePolicyState st;
  quietStep(st, 1000, false);
  TEST_ASSERT_EQUAL(RescuePhase::Ap, st.phase);
  // Hours later: still parked on the AP — a rescue image must never reboot
  // itself out from under a recovery in progress.
  TEST_ASSERT_EQUAL(RescueAction::None, quietStep(st, 1000 + 3600000UL, false));
  TEST_ASSERT_EQUAL(RescueAction::None, quietStep(st, 1000 + 7200000UL, true));
  TEST_ASSERT_EQUAL(RescuePhase::Ap, st.phase);
}

static void test_online_link_drop_stays_online() {
  // Link drops belong to the SDK's auto-reconnect, never a mode change.
  RescuePolicyState st;
  quietStep(st, 1000, true);
  rescuePolicyStep(st, 2000, true, true);  // -> Online
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
  RUN_TEST(test_ap_is_terminal_no_timeout_no_reboot);
  RUN_TEST(test_online_link_drop_stays_online);
  RUN_TEST(test_join_deadline_is_rollover_safe);
  return UNITY_END();
}
