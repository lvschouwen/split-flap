// Host-side unit tests for StatusLedPolicy.h (#199) — the pure state→color
// decision behind the onboard WS2812 status LED. Priority: an in-flight OTA
// upload outranks an unconfirmed PENDING_VERIFY image outranks connectivity;
// a healthy connection breathes dim green (living-room device, user call
// 2026-07-10), everything demanding attention is bright.

#include <ArduinoFake.h>
#include <unity.h>

#include "../../StatusLedPolicy.h"

void setUp() {}
void tearDown() {}

static StatusLedInput makeInput(WifiPhase phase, bool credsStored = false) {
  StatusLedInput in;
  in.wifiPhase = phase;
  in.credsStored = credsStored;
  return in;
}

static void assertColor(uint8_t r, uint8_t g, uint8_t b,
                        const StatusLedColor& c) {
  TEST_ASSERT_EQUAL_UINT8(r, c.r);
  TEST_ASSERT_EQUAL_UINT8(g, c.g);
  TEST_ASSERT_EQUAL_UINT8(b, c.b);
}

// --- connectivity states -------------------------------------------------------

static void test_boot_is_white() {
  StatusLedColor c = decideStatusLed(makeInput(WifiPhase::Boot));
  assertColor(STATUS_LED_ATTENTION, STATUS_LED_ATTENTION, STATUS_LED_ATTENTION,
              c);
}

static void test_joining_is_yellow() {
  StatusLedColor c = decideStatusLed(makeInput(WifiPhase::Joining, true));
  assertColor(STATUS_LED_ATTENTION, 48, 0, c);
}

static void test_portal_unprovisioned_is_blue() {
  // First-boot setup portal: nothing is wrong, the device just needs
  // credentials.
  StatusLedColor c = decideStatusLed(makeInput(WifiPhase::Portal, false));
  assertColor(0, 0, STATUS_LED_ATTENTION, c);
}

static void test_portal_with_stored_creds_is_red() {
  // Portal despite stored credentials = the join failed and the device is
  // cycling toward the reboot-retry — the fault state.
  StatusLedColor c = decideStatusLed(makeInput(WifiPhase::Portal, true));
  assertColor(STATUS_LED_ATTENTION, 0, 0, c);
}

// --- OTA states outrank connectivity -------------------------------------------

static void test_upload_active_is_purple_over_any_phase() {
  StatusLedInput in = makeInput(WifiPhase::Connected, true);
  in.otaUploadActive = true;
  assertColor(STATUS_LED_ATTENTION, 0, STATUS_LED_ATTENTION,
              decideStatusLed(in));

  in = makeInput(WifiPhase::Portal, true);  // even over the fault red
  in.otaUploadActive = true;
  assertColor(STATUS_LED_ATTENTION, 0, STATUS_LED_ATTENTION,
              decideStatusLed(in));
}

static void test_pending_verify_is_orange_over_wifi_phase() {
  StatusLedInput in = makeInput(WifiPhase::Joining, true);
  in.otaPendingVerify = true;
  assertColor(STATUS_LED_ATTENTION, 12, 0, decideStatusLed(in));
}

static void test_upload_beats_pending_verify() {
  // Flash writes in progress are the thing to never interrupt — show them
  // even while this boot's own image is still unconfirmed.
  StatusLedInput in = makeInput(WifiPhase::Connected);
  in.otaUploadActive = true;
  in.otaPendingVerify = true;
  assertColor(STATUS_LED_ATTENTION, 0, STATUS_LED_ATTENTION,
              decideStatusLed(in));
}

// --- healthy heartbeat ----------------------------------------------------------

static void test_connected_breathes_green_between_min_and_max() {
  StatusLedInput in = makeInput(WifiPhase::Connected, true);

  in.nowMs = 0;  // trough of the triangle wave
  StatusLedColor c = decideStatusLed(in);
  assertColor(0, STATUS_LED_HEARTBEAT_MIN, 0, c);

  in.nowMs = STATUS_LED_BREATH_PERIOD_MS / 2;  // crest
  c = decideStatusLed(in);
  assertColor(0, STATUS_LED_HEARTBEAT_MAX, 0, c);

  in.nowMs = STATUS_LED_BREATH_PERIOD_MS;  // next trough (wraps clean)
  c = decideStatusLed(in);
  assertColor(0, STATUS_LED_HEARTBEAT_MIN, 0, c);
}

static void test_connected_ramp_is_symmetric_and_bounded() {
  StatusLedInput in = makeInput(WifiPhase::Connected, true);
  const uint32_t q = STATUS_LED_BREATH_PERIOD_MS / 4;

  in.nowMs = q;  // rising flank
  StatusLedColor rising = decideStatusLed(in);
  in.nowMs = 3 * q;  // falling flank, same height
  StatusLedColor falling = decideStatusLed(in);
  TEST_ASSERT_EQUAL_UINT8(rising.g, falling.g);
  TEST_ASSERT_TRUE(rising.g > STATUS_LED_HEARTBEAT_MIN);
  TEST_ASSERT_TRUE(rising.g < STATUS_LED_HEARTBEAT_MAX);
  TEST_ASSERT_EQUAL_UINT8(0, rising.r);
  TEST_ASSERT_EQUAL_UINT8(0, rising.b);
}

static void test_connected_heartbeat_survives_millis_rollover() {
  // nowMs % period is well-defined across the uint32 wrap; just pin that
  // the color stays inside the dim band at the edge values.
  StatusLedInput in = makeInput(WifiPhase::Connected, true);
  in.nowMs = 0xFFFFFFFFu;
  StatusLedColor c = decideStatusLed(in);
  TEST_ASSERT_TRUE(c.g >= STATUS_LED_HEARTBEAT_MIN);
  TEST_ASSERT_TRUE(c.g <= STATUS_LED_HEARTBEAT_MAX);
}

// --- change detection helper ----------------------------------------------------

static void test_color_equality_helper() {
  StatusLedColor a{1, 2, 3}, b{1, 2, 3}, d{1, 2, 4};
  TEST_ASSERT_TRUE(statusLedColorEq(a, b));
  TEST_ASSERT_FALSE(statusLedColorEq(a, d));
}

// ---------------------------------------------------------------------------

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_boot_is_white);
  RUN_TEST(test_joining_is_yellow);
  RUN_TEST(test_portal_unprovisioned_is_blue);
  RUN_TEST(test_portal_with_stored_creds_is_red);
  RUN_TEST(test_upload_active_is_purple_over_any_phase);
  RUN_TEST(test_pending_verify_is_orange_over_wifi_phase);
  RUN_TEST(test_upload_beats_pending_verify);
  RUN_TEST(test_connected_breathes_green_between_min_and_max);
  RUN_TEST(test_connected_ramp_is_symmetric_and_bounded);
  RUN_TEST(test_connected_heartbeat_survives_millis_rollover);
  RUN_TEST(test_color_equality_helper);
  return UNITY_END();
}
