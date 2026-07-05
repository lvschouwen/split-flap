// Host-side unit tests for the pure MQTT logic in MqttHelpers.h (#121).
// No networking, no AsyncMqttClient — only parsing, state, and JSON assembly.

#include <ArduinoFake.h>
#include <unity.h>
#include <cstring>
#include "../../MqttHelpers.h"

using namespace fakeit;

void setUp() {
  ArduinoFakeReset();
}
void tearDown() {}

// ---- clampDwellSeconds ----
static void test_clampDwell_passes_through_in_range() {
  TEST_ASSERT_EQUAL_INT32(60, clampDwellSeconds(60));
  TEST_ASSERT_EQUAL_INT32(5, clampDwellSeconds(5));
  TEST_ASSERT_EQUAL_INT32(3600, clampDwellSeconds(3600));
}
static void test_clampDwell_clamps_out_of_range() {
  TEST_ASSERT_EQUAL_INT32(5, clampDwellSeconds(0));
  TEST_ASSERT_EQUAL_INT32(5, clampDwellSeconds(-100));
  TEST_ASSERT_EQUAL_INT32(3600, clampDwellSeconds(99999));
}

// ---- parseMqttTextPayload ----
static void test_parse_plain_text_returns_false() {
  String text; long dwell = -1;
  TEST_ASSERT_FALSE(parseMqttTextPayload(String("HELLO WORLD"), text, dwell));
}
static void test_parse_valid_json_text_only_uses_default_dwell() {
  String text; long dwell = -1;
  TEST_ASSERT_TRUE(parseMqttTextPayload(String("{\"text\":\"DOORBELL\"}"), text, dwell));
  TEST_ASSERT_EQUAL_STRING("DOORBELL", text.c_str());
  TEST_ASSERT_EQUAL_INT32(MQTT_TEXT_DWELL_S, dwell);
}
static void test_parse_valid_json_with_dwell() {
  String text; long dwell = -1;
  TEST_ASSERT_TRUE(parseMqttTextPayload(String("{\"text\":\"HI\",\"dwell\":120}"), text, dwell));
  TEST_ASSERT_EQUAL_STRING("HI", text.c_str());
  TEST_ASSERT_EQUAL_INT32(120, dwell);
}
static void test_parse_dwell_is_clamped() {
  String text; long dwell = -1;
  TEST_ASSERT_TRUE(parseMqttTextPayload(String("{\"text\":\"A\",\"dwell\":1}"), text, dwell));
  TEST_ASSERT_EQUAL_INT32(MQTT_DWELL_MIN_S, dwell);
  TEST_ASSERT_TRUE(parseMqttTextPayload(String("{\"text\":\"A\",\"dwell\":99999}"), text, dwell));
  TEST_ASSERT_EQUAL_INT32(MQTT_DWELL_MAX_S, dwell);
}
static void test_parse_whitespace_tolerant() {
  String text; long dwell = -1;
  TEST_ASSERT_TRUE(parseMqttTextPayload(String("  { \"text\" : \"SPACED\" , \"dwell\" : 30 }  "), text, dwell));
  TEST_ASSERT_EQUAL_STRING("SPACED", text.c_str());
  TEST_ASSERT_EQUAL_INT32(30, dwell);
}
static void test_parse_escaped_quote_and_backslash() {
  String text; long dwell = -1;
  TEST_ASSERT_TRUE(parseMqttTextPayload(String("{\"text\":\"A\\\"B\\\\C\"}"), text, dwell));
  TEST_ASSERT_EQUAL_STRING("A\"B\\C", text.c_str());
}
static void test_parse_escaped_newline() {
  String text; long dwell = -1;
  TEST_ASSERT_TRUE(parseMqttTextPayload(String("{\"text\":\"LINE1\\nLINE2\"}"), text, dwell));
  TEST_ASSERT_EQUAL_STRING("LINE1\nLINE2", text.c_str());
}
static void test_parse_malformed_json_returns_false() {
  String text; long dwell = -1;
  // Starts with { but is garbage — caller must fall back to plain text.
  TEST_ASSERT_FALSE(parseMqttTextPayload(String("{garbage"), text, dwell));
  TEST_ASSERT_FALSE(parseMqttTextPayload(String("{\"dwell\":30}"), text, dwell));        // no "text" key
  TEST_ASSERT_FALSE(parseMqttTextPayload(String("{\"text\":\"UNTERMINATED}"), text, dwell)); // unterminated string
  TEST_ASSERT_FALSE(parseMqttTextPayload(String("{\"text\":42}"), text, dwell));          // non-string text
}
static void test_parse_empty_text_is_valid() {
  String text = "x"; long dwell = -1;
  TEST_ASSERT_TRUE(parseMqttTextPayload(String("{\"text\":\"\"}"), text, dwell));
  TEST_ASSERT_EQUAL_STRING("", text.c_str());
}

// ---- MqttNotification state machine ----
static void test_notification_inactive_by_default() {
  MqttNotification n;
  TEST_ASSERT_FALSE(notificationTick(n, 1000UL));
}
static void test_notification_active_until_deadline() {
  MqttNotification n;
  notificationStart(n, String("DOORBELL"), 60, 10000UL);
  TEST_ASSERT_TRUE(notificationTick(n, 10000UL));
  TEST_ASSERT_TRUE(notificationTick(n, 10000UL + 59999UL));
  TEST_ASSERT_EQUAL_STRING("DOORBELL", n.text.c_str());
}
static void test_notification_expires_at_deadline() {
  MqttNotification n;
  notificationStart(n, String("DOORBELL"), 60, 10000UL);
  TEST_ASSERT_FALSE(notificationTick(n, 10000UL + 60000UL));
  // and stays expired
  TEST_ASSERT_FALSE(notificationTick(n, 10000UL + 60001UL));
  TEST_ASSERT_FALSE(n.active);
}
static void test_notification_replacement_resets_deadline() {
  MqttNotification n;
  notificationStart(n, String("FIRST"), 60, 0UL);
  notificationStart(n, String("SECOND"), 60, 50000UL);
  TEST_ASSERT_TRUE(notificationTick(n, 100000UL));  // 60 s past FIRST's start, but SECOND runs to 110 s
  TEST_ASSERT_EQUAL_STRING("SECOND", n.text.c_str());
  TEST_ASSERT_FALSE(notificationTick(n, 110000UL));
}
static void test_notification_start_clamps_dwell() {
  MqttNotification n;
  notificationStart(n, String("X"), 999999, 0UL);
  TEST_ASSERT_TRUE(notificationTick(n, 3599999UL));   // 3600 s - 1 ms
  TEST_ASSERT_FALSE(notificationTick(n, 3600000UL));
}
static void test_notification_survives_millis_wraparound() {
  MqttNotification n;
  // Start 5 s before millis() wraps; a 60 s dwell must stay active across 0.
  unsigned long nearWrap = 0xFFFFFFFFUL - 5000UL;
  notificationStart(n, String("WRAP"), 60, nearWrap);
  TEST_ASSERT_TRUE(notificationTick(n, 0xFFFFFFFFUL));
  TEST_ASSERT_TRUE(notificationTick(n, 10000UL));    // 15 s in, post-wrap
  TEST_ASSERT_FALSE(notificationTick(n, 55001UL));   // 60.001 s in
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_clampDwell_passes_through_in_range);
  RUN_TEST(test_clampDwell_clamps_out_of_range);
  RUN_TEST(test_parse_plain_text_returns_false);
  RUN_TEST(test_parse_valid_json_text_only_uses_default_dwell);
  RUN_TEST(test_parse_valid_json_with_dwell);
  RUN_TEST(test_parse_dwell_is_clamped);
  RUN_TEST(test_parse_whitespace_tolerant);
  RUN_TEST(test_parse_escaped_quote_and_backslash);
  RUN_TEST(test_parse_escaped_newline);
  RUN_TEST(test_parse_malformed_json_returns_false);
  RUN_TEST(test_parse_empty_text_is_valid);
  RUN_TEST(test_notification_inactive_by_default);
  RUN_TEST(test_notification_active_until_deadline);
  RUN_TEST(test_notification_expires_at_deadline);
  RUN_TEST(test_notification_replacement_resets_deadline);
  RUN_TEST(test_notification_start_clamps_dwell);
  RUN_TEST(test_notification_survives_millis_wraparound);
  return UNITY_END();
}
