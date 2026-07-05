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

// ---- topics / telemetry / discovery ----
static void test_mqttTopic_builds_expected_paths() {
  TEST_ASSERT_EQUAL_STRING("splitflap/flappy/text/set", mqttTopic(String("flappy"), "text/set").c_str());
  TEST_ASSERT_EQUAL_STRING("splitflap/flappy/availability", mqttTopic(String("flappy"), "availability").c_str());
  TEST_ASSERT_EQUAL_STRING("splitflap/flappy/telemetry", mqttTopic(String("flappy"), "telemetry").c_str());
}
static void test_telemetry_payload_exact_shape() {
  char buf[96];
  size_t n = buildTelemetryPayload(buf, sizeof(buf), 25048UL, -61L, 2);
  TEST_ASSERT_EQUAL_STRING("{\"heap\":25048,\"rssi\":-61,\"unitErrors\":2}", buf);
  TEST_ASSERT_EQUAL_UINT32(strlen(buf), (uint32_t)n);
}
// ---- parseModeCommand (#130) ----
static void test_mode_command_accepts_exact_options() {
  TEST_ASSERT_EQUAL_STRING("text", parseModeCommand(String("text")).c_str());
  TEST_ASSERT_EQUAL_STRING("clock", parseModeCommand(String("clock")).c_str());
}
static void test_mode_command_trims_whitespace() {
  TEST_ASSERT_EQUAL_STRING("clock", parseModeCommand(String(" clock\n")).c_str());
}
static void test_mode_command_rejects_case_variants_and_garbage() {
  // HA's select publishes the option strings verbatim — anything else is
  // not from our select and must be ignored, not coerced.
  TEST_ASSERT_EQUAL_STRING("", parseModeCommand(String("TEXT")).c_str());
  TEST_ASSERT_EQUAL_STRING("", parseModeCommand(String("Clock")).c_str());
  TEST_ASSERT_EQUAL_STRING("", parseModeCommand(String("date")).c_str());
  TEST_ASSERT_EQUAL_STRING("", parseModeCommand(String("")).c_str());
}

// ---- notificationCancel (#130) ----
static void test_notification_cancel_mid_dwell() {
  MqttNotification n;
  notificationStart(n, String("DOORBELL"), 60, 1000);
  TEST_ASSERT_TRUE(notificationTick(n, 2000));
  notificationCancel(n);
  TEST_ASSERT_FALSE(notificationTick(n, 2000));
  TEST_ASSERT_EQUAL_STRING("", n.text.c_str());
}

static void test_discovery_topics_per_entity() {
  char buf[64];
  buildDiscoveryTopic(buf, sizeof(buf), DISCOVERY_TEXT, "flappy");
  TEST_ASSERT_EQUAL_STRING("homeassistant/text/flappy/config", buf);
  buildDiscoveryTopic(buf, sizeof(buf), DISCOVERY_HEAP, "flappy");
  TEST_ASSERT_EQUAL_STRING("homeassistant/sensor/flappy_heap/config", buf);
  buildDiscoveryTopic(buf, sizeof(buf), DISCOVERY_RSSI, "flappy");
  TEST_ASSERT_EQUAL_STRING("homeassistant/sensor/flappy_rssi/config", buf);
  buildDiscoveryTopic(buf, sizeof(buf), DISCOVERY_UNIT_ERRORS, "flappy");
  TEST_ASSERT_EQUAL_STRING("homeassistant/sensor/flappy_unit_errors/config", buf);
}
// The discovery payloads are fixed-shape; assert the load-bearing fragments
// rather than byte-for-byte JSON, so cosmetic edits don't churn tests.
static void assert_contains(const char* haystack, const char* needle) {
  if (strstr(haystack, needle) == nullptr) {
    TEST_FAIL_MESSAGE(needle);
  }
}
static void test_discovery_text_payload_fragments() {
  char buf[512];
  size_t n = buildDiscoveryPayload(buf, sizeof(buf), DISCOVERY_TEXT, "flappy", "abc1234");
  TEST_ASSERT_TRUE(n > 0 && n < sizeof(buf));  // no truncation
  TEST_ASSERT_EQUAL_CHAR('{', buf[0]);
  TEST_ASSERT_EQUAL_CHAR('}', buf[n - 1]);
  assert_contains(buf, "\"cmd_t\":\"splitflap/flappy/text/set\"");
  assert_contains(buf, "\"avty_t\":\"splitflap/flappy/availability\"");
  assert_contains(buf, "\"uniq_id\":\"flappy_text\"");
  assert_contains(buf, "\"ids\":[\"flappy\"]");
  assert_contains(buf, "\"sw\":\"abc1234\"");
}
static void test_discovery_sensor_payload_fragments() {
  char buf[512];
  size_t n = buildDiscoveryPayload(buf, sizeof(buf), DISCOVERY_HEAP, "flappy", "abc1234");
  TEST_ASSERT_TRUE(n > 0 && n < sizeof(buf));
  assert_contains(buf, "\"stat_t\":\"splitflap/flappy/telemetry\"");
  assert_contains(buf, "\"val_tpl\":\"{{ value_json.heap }}\"");
  assert_contains(buf, "\"uniq_id\":\"flappy_heap\"");

  buildDiscoveryPayload(buf, sizeof(buf), DISCOVERY_RSSI, "flappy", "abc1234");
  assert_contains(buf, "\"val_tpl\":\"{{ value_json.rssi }}\"");
  assert_contains(buf, "\"dev_cla\":\"signal_strength\"");

  buildDiscoveryPayload(buf, sizeof(buf), DISCOVERY_UNIT_ERRORS, "flappy", "abc1234");
  assert_contains(buf, "\"val_tpl\":\"{{ value_json.unitErrors }}\"");
}

static void test_discovery_mode_select_topic_and_payload() {
  char buf[512];
  buildDiscoveryTopic(buf, sizeof(buf), DISCOVERY_MODE, "flappy");
  TEST_ASSERT_EQUAL_STRING("homeassistant/select/flappy/config", buf);

  size_t n = buildDiscoveryPayload(buf, sizeof(buf), DISCOVERY_MODE, "flappy", "abc1234");
  TEST_ASSERT_TRUE(n > 0 && n < sizeof(buf));
  TEST_ASSERT_EQUAL_CHAR('{', buf[0]);
  TEST_ASSERT_EQUAL_CHAR('}', buf[n - 1]);
  assert_contains(buf, "\"name\":\"Mode\"");
  assert_contains(buf, "\"cmd_t\":\"splitflap/flappy/mode/set\"");
  assert_contains(buf, "\"stat_t\":\"splitflap/flappy/mode\"");
  assert_contains(buf, "\"avty_t\":\"splitflap/flappy/availability\"");
  assert_contains(buf, "\"uniq_id\":\"flappy_mode\"");
  assert_contains(buf, "\"ops\":[\"text\",\"clock\"]");
  assert_contains(buf, "\"ids\":[\"flappy\"]");
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
  RUN_TEST(test_mqttTopic_builds_expected_paths);
  RUN_TEST(test_telemetry_payload_exact_shape);
  RUN_TEST(test_mode_command_accepts_exact_options);
  RUN_TEST(test_mode_command_trims_whitespace);
  RUN_TEST(test_mode_command_rejects_case_variants_and_garbage);
  RUN_TEST(test_notification_cancel_mid_dwell);
  RUN_TEST(test_discovery_topics_per_entity);
  RUN_TEST(test_discovery_text_payload_fragments);
  RUN_TEST(test_discovery_sensor_payload_fragments);
  RUN_TEST(test_discovery_mode_select_topic_and_payload);
  return UNITY_END();
}
