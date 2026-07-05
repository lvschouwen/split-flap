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
  return UNITY_END();
}
