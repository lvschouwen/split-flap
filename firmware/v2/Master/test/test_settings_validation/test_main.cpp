// Host-side unit tests for SettingsValidation.h (#153) — the pure per-field
// validators behind the POST / settings handler. Slot-length arguments use
// the REAL EEPROM layout constants so a layout change that would break
// validation fails here, not on the device.

#include <ArduinoFake.h>
#include <unity.h>
#include "../../SettingsValidation.h"
#include "../../SettingsLimits.h"

void setUp() {
  ArduinoFakeReset();
}
void tearDown() {}

// --- alignment / device mode ---------------------------------------------

static void test_alignment_accepts_the_three_modes() {
  TEST_ASSERT_TRUE(isValidAlignmentValue(String("left")));
  TEST_ASSERT_TRUE(isValidAlignmentValue(String("center")));
  TEST_ASSERT_TRUE(isValidAlignmentValue(String("right")));
}

static void test_alignment_rejects_case_empty_and_unknown() {
  TEST_ASSERT_FALSE(isValidAlignmentValue(String("Left")));
  TEST_ASSERT_FALSE(isValidAlignmentValue(String("")));
  TEST_ASSERT_FALSE(isValidAlignmentValue(String("top")));
}

static void test_device_mode_accepts_text_and_clock() {
  TEST_ASSERT_TRUE(isValidDeviceModeValue(String("text")));
  TEST_ASSERT_TRUE(isValidDeviceModeValue(String("clock")));
}

static void test_device_mode_rejects_case_empty_and_unknown() {
  TEST_ASSERT_FALSE(isValidDeviceModeValue(String("TEXT")));
  TEST_ASSERT_FALSE(isValidDeviceModeValue(String("")));
  TEST_ASSERT_FALSE(isValidDeviceModeValue(String("off")));
}

// --- flap speed ------------------------------------------------------------

static void test_flap_speed_accepts_bounds_and_midrange() {
  TEST_ASSERT_TRUE(isValidFlapSpeedValue(String("1")));
  TEST_ASSERT_TRUE(isValidFlapSpeedValue(String("50")));
  TEST_ASSERT_TRUE(isValidFlapSpeedValue(String("100")));
}

static void test_flap_speed_rejects_out_of_range_and_garbage() {
  TEST_ASSERT_FALSE(isValidFlapSpeedValue(String("0")));
  TEST_ASSERT_FALSE(isValidFlapSpeedValue(String("101")));
  TEST_ASSERT_FALSE(isValidFlapSpeedValue(String("-1")));
  TEST_ASSERT_FALSE(isValidFlapSpeedValue(String("abc")));
  TEST_ASSERT_FALSE(isValidFlapSpeedValue(String("")));
  TEST_ASSERT_FALSE(isValidFlapSpeedValue(String("5x")));
}

static void test_flap_speed_keeps_historic_strtol_semantics() {
  // isNumber() has always been strtol-based: leading whitespace parses.
  // The #153 extraction must not silently tighten the accepted inputs —
  // this pins the (quirky but harmless) historic behaviour.
  TEST_ASSERT_TRUE(isValidFlapSpeedValue(String(" 5")));
}

// --- timezone ----------------------------------------------------------------

static void test_timezone_accepts_empty_and_real_posix_string() {
  TEST_ASSERT_TRUE(isValidTimezoneValue(String(""), LEN_TIMEZONE));
  TEST_ASSERT_TRUE(isValidTimezoneValue(String("CET-1CEST,M3.5.0,M10.5.0/3"), LEN_TIMEZONE));
  TEST_ASSERT_TRUE(isValidTimezoneValue(String("<+0330>-3:30"), LEN_TIMEZONE));
}

static void test_timezone_rejects_control_chars_and_overflow() {
  String withControl = "CET";
  withControl += (char)0x1F;
  TEST_ASSERT_FALSE(isValidTimezoneValue(withControl, LEN_TIMEZONE));
  String withDel = "CET";
  withDel += (char)0x7F;
  TEST_ASSERT_FALSE(isValidTimezoneValue(withDel, LEN_TIMEZONE));

  String tooLong;
  for (int i = 0; i < LEN_TIMEZONE; i++) tooLong += 'A';  // == slot len
  TEST_ASSERT_FALSE(isValidTimezoneValue(tooLong, LEN_TIMEZONE));
  TEST_ASSERT_TRUE(isValidTimezoneValue(tooLong.substring(1), LEN_TIMEZONE));
}

// --- mqtt host / user ------------------------------------------------------

static void test_mqtt_host_user_accepts_hostnames_and_empty() {
  TEST_ASSERT_TRUE(isValidMqttHostOrUserValue(String("broker.local"), LEN_MQTT_HOST));
  TEST_ASSERT_TRUE(isValidMqttHostOrUserValue(String("192.168.1.10"), LEN_MQTT_HOST));
  TEST_ASSERT_TRUE(isValidMqttHostOrUserValue(String(""), LEN_MQTT_HOST));  // empty host = MQTT off
  TEST_ASSERT_TRUE(isValidMqttHostOrUserValue(String("ha-user_1"), LEN_MQTT_USER));
}

static void test_mqtt_host_user_rejects_spaces_controls_overflow() {
  TEST_ASSERT_FALSE(isValidMqttHostOrUserValue(String("bro ker"), LEN_MQTT_HOST));  // 0x20 excluded
  String withControl = "broker";
  withControl += (char)0x09;
  TEST_ASSERT_FALSE(isValidMqttHostOrUserValue(withControl, LEN_MQTT_HOST));
  String tooLong;
  for (int i = 0; i < LEN_MQTT_USER; i++) tooLong += 'u';  // == slot len
  TEST_ASSERT_FALSE(isValidMqttHostOrUserValue(tooLong, LEN_MQTT_USER));
}

// --- mqtt port --------------------------------------------------------------

static void test_mqtt_port_accepts_empty_and_valid_range() {
  TEST_ASSERT_TRUE(isValidMqttPortValue(String(""), LEN_MQTT_PORT));  // default 1883
  TEST_ASSERT_TRUE(isValidMqttPortValue(String("1"), LEN_MQTT_PORT));
  TEST_ASSERT_TRUE(isValidMqttPortValue(String("1883"), LEN_MQTT_PORT));
  TEST_ASSERT_TRUE(isValidMqttPortValue(String("65535"), LEN_MQTT_PORT));
}

static void test_mqtt_port_rejects_zero_overflow_garbage() {
  TEST_ASSERT_FALSE(isValidMqttPortValue(String("0"), LEN_MQTT_PORT));
  TEST_ASSERT_FALSE(isValidMqttPortValue(String("65536"), LEN_MQTT_PORT));
  TEST_ASSERT_FALSE(isValidMqttPortValue(String("abc"), LEN_MQTT_PORT));
}

static void test_mqtt_port_rejects_slot_truncating_zero_padding() {
  // "0001883" parses to 1883 but would truncate to "00018" in the 6-byte
  // slot — the length bound must reject it (#57 reviewer finding).
  TEST_ASSERT_FALSE(isValidMqttPortValue(String("0001883"), LEN_MQTT_PORT));
}

// --- mqtt password -----------------------------------------------------------

static void test_mqtt_password_accepts_printables_including_spaces() {
  TEST_ASSERT_TRUE(isValidMqttPasswordValue(String("hunter2"), LEN_MQTT_PASSWORD));
  TEST_ASSERT_TRUE(isValidMqttPasswordValue(String("pass with spaces!"), LEN_MQTT_PASSWORD));
}

static void test_mqtt_password_rejects_controls_and_overflow() {
  String withControl = "pw";
  withControl += (char)0x0A;
  TEST_ASSERT_FALSE(isValidMqttPasswordValue(withControl, LEN_MQTT_PASSWORD));
  String tooLong;
  for (int i = 0; i < LEN_MQTT_PASSWORD; i++) tooLong += 'p';  // == slot len
  TEST_ASSERT_FALSE(isValidMqttPasswordValue(tooLong, LEN_MQTT_PASSWORD));
}

// ---- transient dwell (#176) ----

static void test_transient_dwell_accepts_empty_and_valid_range() {
  // Empty = caller applies the 600 s default.
  TEST_ASSERT_TRUE(isValidTransientDwellValue(String("")));
  TEST_ASSERT_TRUE(isValidTransientDwellValue(String("5")));
  TEST_ASSERT_TRUE(isValidTransientDwellValue(String("600")));
  TEST_ASSERT_TRUE(isValidTransientDwellValue(String("3600")));
}

static void test_transient_dwell_rejects_out_of_range_and_garbage() {
  // Bounds mirror clampDwellSeconds' [5, 3600] so what validates is what
  // runs — a value the clamp would alter is rejected instead.
  TEST_ASSERT_FALSE(isValidTransientDwellValue(String("4")));
  TEST_ASSERT_FALSE(isValidTransientDwellValue(String("0")));
  TEST_ASSERT_FALSE(isValidTransientDwellValue(String("-60")));
  TEST_ASSERT_FALSE(isValidTransientDwellValue(String("3601")));
  TEST_ASSERT_FALSE(isValidTransientDwellValue(String("abc")));
  TEST_ASSERT_FALSE(isValidTransientDwellValue(String("60s")));
}

// --- wifi ssid / password (#188) ---------------------------------------------

static void test_wifi_ssid_accepts_lengths_and_spaces() {
  TEST_ASSERT_TRUE(isValidWifiSsidValue(String("a"), LEN_WIFI_SSID));
  TEST_ASSERT_TRUE(isValidWifiSsidValue(String("My Home WiFi"), LEN_WIFI_SSID));
  String max32;
  for (int i = 0; i < 32; i++) max32 += 's';  // 802.11 SSID ceiling
  TEST_ASSERT_TRUE(isValidWifiSsidValue(max32, LEN_WIFI_SSID));
}

static void test_wifi_ssid_rejects_empty_overflow_controls() {
  // Empty means "unprovisioned" internally — never a valid submission.
  TEST_ASSERT_FALSE(isValidWifiSsidValue(String(""), LEN_WIFI_SSID));
  String tooLong;
  for (int i = 0; i < 33; i++) tooLong += 's';
  TEST_ASSERT_FALSE(isValidWifiSsidValue(tooLong, LEN_WIFI_SSID));
  String withControl = "net";
  withControl += (char)0x1B;
  TEST_ASSERT_FALSE(isValidWifiSsidValue(withControl, LEN_WIFI_SSID));
}

static void test_wifi_password_accepts_empty_and_wpa2_lengths() {
  // Empty = open network. Otherwise WPA2-PSK bounds: 8..63 chars.
  TEST_ASSERT_TRUE(isValidWifiPasswordValue(String(""), LEN_WIFI_PASSWORD));
  TEST_ASSERT_TRUE(isValidWifiPasswordValue(String("12345678"), LEN_WIFI_PASSWORD));
  TEST_ASSERT_TRUE(isValidWifiPasswordValue(String("pass with spaces!"), LEN_WIFI_PASSWORD));
  String max63;
  for (int i = 0; i < 63; i++) max63 += 'p';
  TEST_ASSERT_TRUE(isValidWifiPasswordValue(max63, LEN_WIFI_PASSWORD));
}

static void test_wifi_password_rejects_short_overflow_controls() {
  // 1..7 chars can never be a WPA2-PSK — reject at the boundary instead of
  // letting a doomed join eat the 30 s window and land in the portal.
  TEST_ASSERT_FALSE(isValidWifiPasswordValue(String("1234567"), LEN_WIFI_PASSWORD));
  String tooLong;
  for (int i = 0; i < 64; i++) tooLong += 'p';
  TEST_ASSERT_FALSE(isValidWifiPasswordValue(tooLong, LEN_WIFI_PASSWORD));
  String withControl = "password";
  withControl += (char)0x0A;
  TEST_ASSERT_FALSE(isValidWifiPasswordValue(withControl, LEN_WIFI_PASSWORD));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_wifi_ssid_accepts_lengths_and_spaces);
  RUN_TEST(test_wifi_ssid_rejects_empty_overflow_controls);
  RUN_TEST(test_wifi_password_accepts_empty_and_wpa2_lengths);
  RUN_TEST(test_wifi_password_rejects_short_overflow_controls);
  RUN_TEST(test_transient_dwell_accepts_empty_and_valid_range);
  RUN_TEST(test_transient_dwell_rejects_out_of_range_and_garbage);
  RUN_TEST(test_alignment_accepts_the_three_modes);
  RUN_TEST(test_alignment_rejects_case_empty_and_unknown);
  RUN_TEST(test_device_mode_accepts_text_and_clock);
  RUN_TEST(test_device_mode_rejects_case_empty_and_unknown);
  RUN_TEST(test_flap_speed_accepts_bounds_and_midrange);
  RUN_TEST(test_flap_speed_rejects_out_of_range_and_garbage);
  RUN_TEST(test_flap_speed_keeps_historic_strtol_semantics);
  RUN_TEST(test_timezone_accepts_empty_and_real_posix_string);
  RUN_TEST(test_timezone_rejects_control_chars_and_overflow);
  RUN_TEST(test_mqtt_host_user_accepts_hostnames_and_empty);
  RUN_TEST(test_mqtt_host_user_rejects_spaces_controls_overflow);
  RUN_TEST(test_mqtt_port_accepts_empty_and_valid_range);
  RUN_TEST(test_mqtt_port_rejects_zero_overflow_garbage);
  RUN_TEST(test_mqtt_port_rejects_slot_truncating_zero_padding);
  RUN_TEST(test_mqtt_password_accepts_printables_including_spaces);
  RUN_TEST(test_mqtt_password_rejects_controls_and_overflow);
  return UNITY_END();
}
