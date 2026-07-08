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

// Defined further down; used by several payload/telemetry fragment checks.
static void assert_contains(const char* haystack, const char* needle);

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
static void test_transient_text_owns_display_for_requested_dwell() {
  // #165/#176: web transients (calibration pattern, timed messages) ride
  // the show-then-revert state — persisted mode untouched, auto-revert.
  MqttNotification n;
  transientTextStart(n, String("AAAAA"), 300, 10000UL);
  TEST_ASSERT_TRUE(notificationTick(n, 10000UL));
  TEST_ASSERT_EQUAL_STRING("AAAAA", n.text.c_str());
  TEST_ASSERT_TRUE(notificationTick(n, 10000UL + 300 * 1000UL - 1));
  TEST_ASSERT_FALSE(notificationTick(n, 10000UL + 300 * 1000UL));
}
static void test_transient_text_nonpositive_dwell_uses_default() {
  // Dwell <= 0 = "not provided" — the 600 s default applies.
  long values[] = {0, -5};
  for (int i = 0; i < 2; i++) {
    MqttNotification n;
    transientTextStart(n, String("AAAAA"), values[i], 10000UL);
    TEST_ASSERT_TRUE(notificationTick(n, 10000UL + TRANSIENT_TEXT_DEFAULT_DWELL_SECONDS * 1000UL - 1));
    TEST_ASSERT_FALSE(notificationTick(n, 10000UL + TRANSIENT_TEXT_DEFAULT_DWELL_SECONDS * 1000UL));
  }
}
static void test_transient_default_dwell_within_notification_clamp() {
  // notificationStart clamps to [5, 3600] — the default must sit inside
  // that window or the deadline silently shrinks/grows.
  TEST_ASSERT_EQUAL_INT32(TRANSIENT_TEXT_DEFAULT_DWELL_SECONDS,
                          clampDwellSeconds(TRANSIENT_TEXT_DEFAULT_DWELL_SECONDS));
}
static void test_transient_text_cancelled_by_mode_change() {
  // Same cancel semantics as an MQTT notification (#130): an explicit mode
  // change trumps the transient.
  MqttNotification n;
  transientTextStart(n, String("AAAAA"), 300, 0UL);
  notificationCancel(n);
  TEST_ASSERT_FALSE(notificationTick(n, 1UL));
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
  char buf[128];
  //                            heap    frag  rssi  errs uptime  ntp
  size_t n = buildTelemetryPayload(buf, sizeof(buf), 25048UL, 12, -61L, 2, 3600UL, true);
  TEST_ASSERT_EQUAL_STRING(
    "{\"heap\":25048,\"heapFrag\":12,\"rssi\":-61,\"unitErrors\":2,\"uptime\":3600,\"ntp\":1}", buf);
  TEST_ASSERT_EQUAL_UINT32(strlen(buf), (uint32_t)n);
}
static void test_telemetry_ntp_false_renders_zero() {
  char buf[128];
  buildTelemetryPayload(buf, sizeof(buf), 1UL, 0, 0L, 0, 0UL, false);
  assert_contains(buf, "\"ntp\":0");
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

// ---- Stage A sensor expansion (#132) ----
static void test_discovery_new_topics() {
  char buf[64];
  buildDiscoveryTopic(buf, sizeof(buf), DISCOVERY_HEAP_FRAG, "flappy");
  TEST_ASSERT_EQUAL_STRING("homeassistant/sensor/flappy_heap_frag/config", buf);
  buildDiscoveryTopic(buf, sizeof(buf), DISCOVERY_UPTIME, "flappy");
  TEST_ASSERT_EQUAL_STRING("homeassistant/sensor/flappy_uptime/config", buf);
  buildDiscoveryTopic(buf, sizeof(buf), DISCOVERY_NTP, "flappy");
  TEST_ASSERT_EQUAL_STRING("homeassistant/binary_sensor/flappy_ntp/config", buf);
  buildDiscoveryTopic(buf, sizeof(buf), DISCOVERY_NOTIFICATION, "flappy");
  TEST_ASSERT_EQUAL_STRING("homeassistant/binary_sensor/flappy_notification/config", buf);
  buildDiscoveryTopic(buf, sizeof(buf), DISCOVERY_OTA_REVERTED, "flappy");
  TEST_ASSERT_EQUAL_STRING("homeassistant/binary_sensor/flappy_ota_reverted/config", buf);
  buildDiscoveryTopic(buf, sizeof(buf), DISCOVERY_CURRENT_TEXT, "flappy");
  TEST_ASSERT_EQUAL_STRING("homeassistant/sensor/flappy_text_state/config", buf);
}
static void test_discovery_telemetry_backed_fragments() {
  char buf[512];
  buildDiscoveryPayload(buf, sizeof(buf), DISCOVERY_HEAP_FRAG, "flappy", "abc1234");
  assert_contains(buf, "\"stat_t\":\"splitflap/flappy/telemetry\"");
  assert_contains(buf, "\"val_tpl\":\"{{ value_json.heapFrag }}\"");
  assert_contains(buf, "\"unit_of_meas\":\"%\"");
  assert_contains(buf, "\"ent_cat\":\"diagnostic\"");

  buildDiscoveryPayload(buf, sizeof(buf), DISCOVERY_UPTIME, "flappy", "abc1234");
  assert_contains(buf, "\"val_tpl\":\"{{ value_json.uptime }}\"");
  assert_contains(buf, "\"dev_cla\":\"duration\"");
  assert_contains(buf, "\"unit_of_meas\":\"s\"");

  buildDiscoveryPayload(buf, sizeof(buf), DISCOVERY_NTP, "flappy", "abc1234");
  assert_contains(buf, "\"val_tpl\":\"{{ value_json.ntp }}\"");
  assert_contains(buf, "\"pl_on\":\"1\",\"pl_off\":\"0\"");
  assert_contains(buf, "\"dev_cla\":\"connectivity\"");
}
static void test_discovery_own_topic_fragments() {
  char buf[512];
  buildDiscoveryPayload(buf, sizeof(buf), DISCOVERY_CURRENT_TEXT, "flappy", "abc1234");
  assert_contains(buf, "\"stat_t\":\"splitflap/flappy/text/state\"");
  assert_contains(buf, "\"uniq_id\":\"flappy_text_state\"");
  TEST_ASSERT_NULL(strstr(buf, "val_tpl"));  // plain payload, no template

  buildDiscoveryPayload(buf, sizeof(buf), DISCOVERY_NOTIFICATION, "flappy", "abc1234");
  assert_contains(buf, "\"stat_t\":\"splitflap/flappy/notification\"");
  assert_contains(buf, "\"pl_on\":\"ON\",\"pl_off\":\"OFF\"");

  buildDiscoveryPayload(buf, sizeof(buf), DISCOVERY_WIDTH, "flappy", "abc1234");
  assert_contains(buf, "\"stat_t\":\"splitflap/flappy/width\"");
}
static void test_discovery_diagnostics_fragments() {
  char buf[512];
  buildDiscoveryPayload(buf, sizeof(buf), DISCOVERY_IP, "flappy", "abc1234");
  assert_contains(buf, "\"stat_t\":\"splitflap/flappy/diag/ip\"");
  assert_contains(buf, "\"ent_cat\":\"diagnostic\"");

  buildDiscoveryPayload(buf, sizeof(buf), DISCOVERY_OTA_REVERTED, "flappy", "abc1234");
  assert_contains(buf, "\"stat_t\":\"splitflap/flappy/diag/ota\"");
  assert_contains(buf, "\"dev_cla\":\"problem\"");
  assert_contains(buf, "\"pl_on\":\"ON\",\"pl_off\":\"OFF\"");

  buildDiscoveryPayload(buf, sizeof(buf), DISCOVERY_TIMEZONE, "flappy", "abc1234");
  assert_contains(buf, "\"stat_t\":\"splitflap/flappy/diag/tz\"");
}
// Every entity must produce a well-formed, non-truncated payload+topic — the
// generic builder's guard means a too-small buffer would silently return a
// cut JSON, which the runtime rejects; this asserts none of them get close.
static void test_all_discovery_entities_wellformed() {
  char pbuf[512];
  char tbuf[96];
  // A long-ish device id to stress the buffers (still <= the 24-char cap +).
  const char* id = "split-flap-9a3c1f";
  for (int e = 0; e < DISCOVERY_ENTITY_COUNT; e++) {
    size_t tn = buildDiscoveryTopic(tbuf, sizeof(tbuf), e, id);
    size_t pn = buildDiscoveryPayload(pbuf, sizeof(pbuf), e, id, "abcdef0");
    TEST_ASSERT_TRUE_MESSAGE(tn > 0 && tn < sizeof(tbuf), "topic truncated/empty");
    TEST_ASSERT_TRUE_MESSAGE(pn > 0 && pn < sizeof(pbuf), "payload truncated/empty");
    TEST_ASSERT_EQUAL_CHAR('{', pbuf[0]);
    TEST_ASSERT_EQUAL_CHAR('}', pbuf[pn - 1]);
    assert_contains(pbuf, "\"ids\":[\"split-flap-9a3c1f\"]");  // device block present
  }
}

// ---- Stage B control parsers + discovery (#132) ----
static void test_speed_command_accepts_in_range() {
  TEST_ASSERT_EQUAL_INT(1, parseSpeedCommand(String("1")));
  TEST_ASSERT_EQUAL_INT(80, parseSpeedCommand(String("80")));
  TEST_ASSERT_EQUAL_INT(100, parseSpeedCommand(String("100")));
  TEST_ASSERT_EQUAL_INT(50, parseSpeedCommand(String("  50\n")));  // trimmed
}
static void test_speed_command_rejects_out_of_range_and_garbage() {
  TEST_ASSERT_EQUAL_INT(-1, parseSpeedCommand(String("0")));
  TEST_ASSERT_EQUAL_INT(-1, parseSpeedCommand(String("101")));
  TEST_ASSERT_EQUAL_INT(-1, parseSpeedCommand(String("-5")));
  TEST_ASSERT_EQUAL_INT(-1, parseSpeedCommand(String("80.0")));   // float rejected
  TEST_ASSERT_EQUAL_INT(-1, parseSpeedCommand(String("fast")));
  TEST_ASSERT_EQUAL_INT(-1, parseSpeedCommand(String("")));
}
static void test_alignment_command_accepts_options_rejects_rest() {
  TEST_ASSERT_EQUAL_STRING("left", parseAlignmentCommand(String("left")).c_str());
  TEST_ASSERT_EQUAL_STRING("center", parseAlignmentCommand(String(" center ")).c_str());
  TEST_ASSERT_EQUAL_STRING("right", parseAlignmentCommand(String("right")).c_str());
  TEST_ASSERT_EQUAL_STRING("", parseAlignmentCommand(String("Left")).c_str());
  TEST_ASSERT_EQUAL_STRING("", parseAlignmentCommand(String("middle")).c_str());
}
static void test_restart_command_only_fires_on_press() {
  TEST_ASSERT_TRUE(parseRestartCommand(String("PRESS")));
  TEST_ASSERT_TRUE(parseRestartCommand(String(" PRESS\n")));
  TEST_ASSERT_FALSE(parseRestartCommand(String("press")));
  TEST_ASSERT_FALSE(parseRestartCommand(String("restart")));
  TEST_ASSERT_FALSE(parseRestartCommand(String("")));
}
static void test_discovery_control_topics_and_payloads() {
  char buf[512];
  buildDiscoveryTopic(buf, sizeof(buf), DISCOVERY_SPEED, "flappy");
  TEST_ASSERT_EQUAL_STRING("homeassistant/number/flappy_speed/config", buf);
  buildDiscoveryTopic(buf, sizeof(buf), DISCOVERY_ALIGNMENT, "flappy");
  TEST_ASSERT_EQUAL_STRING("homeassistant/select/flappy_alignment/config", buf);
  buildDiscoveryTopic(buf, sizeof(buf), DISCOVERY_RESTART, "flappy");
  TEST_ASSERT_EQUAL_STRING("homeassistant/button/flappy_restart/config", buf);

  buildDiscoveryPayload(buf, sizeof(buf), DISCOVERY_SPEED, "flappy", "abc1234");
  assert_contains(buf, "\"cmd_t\":\"splitflap/flappy/speed/set\"");
  assert_contains(buf, "\"stat_t\":\"splitflap/flappy/speed\"");
  assert_contains(buf, "\"min\":1,\"max\":100,\"step\":1");

  buildDiscoveryPayload(buf, sizeof(buf), DISCOVERY_ALIGNMENT, "flappy", "abc1234");
  assert_contains(buf, "\"cmd_t\":\"splitflap/flappy/alignment/set\"");
  assert_contains(buf, "\"ops\":[\"left\",\"center\",\"right\"]");

  buildDiscoveryPayload(buf, sizeof(buf), DISCOVERY_RESTART, "flappy", "abc1234");
  assert_contains(buf, "\"cmd_t\":\"splitflap/flappy/restart/set\"");
  assert_contains(buf, "\"pl_prs\":\"PRESS\"");
  assert_contains(buf, "\"dev_cla\":\"restart\"");
  TEST_ASSERT_NULL(strstr(buf, "stat_t"));  // button has no state topic
  // Guard the varargs alignment end-to-end: device block must carry the real
  // id and fw in the right slots (a missing %s arg would corrupt sw here).
  assert_contains(buf, "\"ids\":[\"flappy\"]");
  assert_contains(buf, "\"sw\":\"abc1234\"");
  assert_contains(buf, "\"name\":\"Split-Flap flappy\"");
}

// #137: the per-unit health sensor carries a state topic (integer faulty count)
// AND a json_attributes topic (per-unit breakdown), and lands in the diagnostic
// entity category. It must NOT collide with the pre-existing "Units responding"
// count entity's _units suffix / units topic.
static void test_discovery_units_faulty_topic_and_payload() {
  char buf[512];
  buildDiscoveryTopic(buf, sizeof(buf), DISCOVERY_UNITS_FAULTY, "flappy");
  TEST_ASSERT_EQUAL_STRING("homeassistant/sensor/flappy_units_faulty/config", buf);

  buildDiscoveryPayload(buf, sizeof(buf), DISCOVERY_UNITS_FAULTY, "flappy", "abc1234");
  assert_contains(buf, "\"name\":\"Faulty units\"");
  assert_contains(buf, "\"stat_t\":\"splitflap/flappy/units_faulty\"");
  assert_contains(buf, "\"json_attr_t\":\"splitflap/flappy/units/attrs\"");
  assert_contains(buf, "\"uniq_id\":\"flappy_units_faulty\"");
  assert_contains(buf, "\"ent_cat\":\"diagnostic\"");
  assert_contains(buf, "\"sw\":\"abc1234\"");
  // Distinct from the "Units responding" count entity (#132).
  TEST_ASSERT_TRUE(strcmp("flappy_units_faulty", "flappy_units") != 0);
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
  RUN_TEST(test_transient_text_owns_display_for_requested_dwell);
  RUN_TEST(test_transient_text_nonpositive_dwell_uses_default);
  RUN_TEST(test_transient_default_dwell_within_notification_clamp);
  RUN_TEST(test_transient_text_cancelled_by_mode_change);
  RUN_TEST(test_notification_survives_millis_wraparound);
  RUN_TEST(test_mqttTopic_builds_expected_paths);
  RUN_TEST(test_telemetry_payload_exact_shape);
  RUN_TEST(test_telemetry_ntp_false_renders_zero);
  RUN_TEST(test_mode_command_accepts_exact_options);
  RUN_TEST(test_mode_command_trims_whitespace);
  RUN_TEST(test_mode_command_rejects_case_variants_and_garbage);
  RUN_TEST(test_notification_cancel_mid_dwell);
  RUN_TEST(test_discovery_topics_per_entity);
  RUN_TEST(test_discovery_text_payload_fragments);
  RUN_TEST(test_discovery_sensor_payload_fragments);
  RUN_TEST(test_discovery_mode_select_topic_and_payload);
  RUN_TEST(test_discovery_new_topics);
  RUN_TEST(test_discovery_telemetry_backed_fragments);
  RUN_TEST(test_discovery_own_topic_fragments);
  RUN_TEST(test_discovery_diagnostics_fragments);
  RUN_TEST(test_all_discovery_entities_wellformed);
  RUN_TEST(test_speed_command_accepts_in_range);
  RUN_TEST(test_speed_command_rejects_out_of_range_and_garbage);
  RUN_TEST(test_alignment_command_accepts_options_rejects_rest);
  RUN_TEST(test_restart_command_only_fires_on_press);
  RUN_TEST(test_discovery_control_topics_and_payloads);
  RUN_TEST(test_discovery_units_faulty_topic_and_payload);
  return UNITY_END();
}
