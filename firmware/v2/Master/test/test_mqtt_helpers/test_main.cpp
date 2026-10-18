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
static void test_parse_unicode_escape_is_decoded() {
  String text; long dwell = -1;
  // Home Assistant's JSON encoder writes every non-ASCII character this way.
  TEST_ASSERT_TRUE(parseMqttTextPayload(String("{\"text\":\"CAF\\u00c9 \\u0041\"}"), text, dwell));
  TEST_ASSERT_EQUAL_STRING("CAF\xC3\x89 A", text.c_str());
}
static void test_parse_reads_the_objects_own_members_only() {
  String text = "x"; long dwell = -1;
  // A "text" inside another member is not the text to show.
  TEST_ASSERT_FALSE(parseMqttTextPayload(String("{\"extra\":{\"text\":\"INNER\"}}"), text, dwell));
  TEST_ASSERT_TRUE(parseMqttTextPayload(
      String("{\"extra\":{\"text\":\"INNER\",\"dwell\":900},\"text\":\"OUTER\"}"), text, dwell));
  TEST_ASSERT_EQUAL_STRING("OUTER", text.c_str());
  TEST_ASSERT_EQUAL(MQTT_TEXT_DWELL_S, dwell);
}
static void test_parse_empty_text_is_valid() {
  String text = "x"; long dwell = -1;
  TEST_ASSERT_TRUE(parseMqttTextPayload(String("{\"text\":\"\"}"), text, dwell));
  TEST_ASSERT_EQUAL_STRING("", text.c_str());
}

// ---- parseMqttTextRequest: hold and priority (#616/#625) ----
static void test_request_defaults_are_a_dwell_at_priority_zero() {
  MqttTextRequest r;
  TEST_ASSERT_TRUE(parseMqttTextRequest(String("{\"text\":\"HI\"}"), r));
  TEST_ASSERT_EQUAL_STRING("HI", r.text.c_str());
  TEST_ASSERT_FALSE(r.hold);
  TEST_ASSERT_EQUAL(0, r.priority);
  TEST_ASSERT_EQUAL_INT32(MQTT_TEXT_DWELL_S, r.dwellSeconds);
}
static void test_request_hold_by_flag_or_zero_dwell() {
  MqttTextRequest r;
  TEST_ASSERT_TRUE(parseMqttTextRequest(String("{\"text\":\"LEAK\",\"hold\":true}"), r));
  TEST_ASSERT_TRUE(r.hold);
  TEST_ASSERT_TRUE(parseMqttTextRequest(String("{\"text\":\"LEAK\",\"dwell\":0}"), r));
  TEST_ASSERT_TRUE(r.hold);
  TEST_ASSERT_TRUE(parseMqttTextRequest(String("{\"text\":\"LEAK\",\"hold\":false,\"dwell\":30}"), r));
  TEST_ASSERT_FALSE(r.hold);
  TEST_ASSERT_EQUAL_INT32(30, r.dwellSeconds);
  // A negative dwell is absent, not a hold.
  TEST_ASSERT_TRUE(parseMqttTextRequest(String("{\"text\":\"A\",\"dwell\":-5}"), r));
  TEST_ASSERT_FALSE(r.hold);
  TEST_ASSERT_EQUAL_INT32(MQTT_TEXT_DWELL_S, r.dwellSeconds);
}
static void test_request_priority_is_clamped_and_force_is_the_top() {
  MqttTextRequest r;
  TEST_ASSERT_TRUE(parseMqttTextRequest(String("{\"text\":\"A\",\"priority\":100}"), r));
  TEST_ASSERT_EQUAL(100, r.priority);
  TEST_ASSERT_TRUE(parseMqttTextRequest(String("{\"text\":\"A\",\"priority\":999}"), r));
  TEST_ASSERT_EQUAL(MQTT_TEXT_PRIORITY_MAX, r.priority);
  TEST_ASSERT_TRUE(parseMqttTextRequest(String("{\"text\":\"A\",\"priority\":-3}"), r));
  TEST_ASSERT_EQUAL(0, r.priority);
  TEST_ASSERT_TRUE(parseMqttTextRequest(String("{\"text\":\"A\",\"force\":true,\"priority\":1}"), r));
  TEST_ASSERT_EQUAL(MQTT_TEXT_PRIORITY_MAX, r.priority);
  TEST_ASSERT_TRUE(parseMqttTextRequest(String("{\"text\":\"A\",\"force\":false}"), r));
  TEST_ASSERT_EQUAL(0, r.priority);
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
static void test_notification_end_is_a_clock_time_once_the_clock_is_set() {
  // 5 min from 12 Oct 2026 10:00:00 UTC.
  TEST_ASSERT_EQUAL_UINT32(1791799200UL + 300UL, notificationEndsEpoch(1791799200UL, true, 300));
  // The dwell is clamped the way the deadline is.
  TEST_ASSERT_EQUAL_UINT32(1791799200UL + (uint32_t)clampDwellSeconds(999999),
                           notificationEndsEpoch(1791799200UL, true, 999999));
  // No clock yet: when it ends cannot be said.
  TEST_ASSERT_EQUAL_UINT32(0, notificationEndsEpoch(42, false, 300));
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
  char buf[160];
  //                            heap    frag  rssi  errs uptime  ntp   vccMin reboots jam   stepsExcess
  size_t n = buildTelemetryPayload(buf, sizeof(buf), 25048UL, 12, -61L, 2, 3600UL, true, 4210, 6UL, true, 340);
  TEST_ASSERT_EQUAL_STRING(
    "{\"heap\":25048,\"heapFrag\":12,\"rssi\":-61,\"unitErrors\":2,\"uptime\":3600,\"ntp\":1,\"vccMin\":4210,\"reboots\":6,\"jam\":\"ON\",\"stepsExcess\":340}", buf);
  TEST_ASSERT_EQUAL_UINT32(strlen(buf), (uint32_t)n);
}
// #623: the weakest unit and the count under the floor ride beside vccMin.
static void test_telemetry_supply_facts_follow_vccmin() {
  char buf[MQTT_TELEMETRY_MAX];
  MqttSupplyFacts supply;
  supply.unit = 0x27;
  supply.board = "split-flap-row1";
  supply.under = 2;
  supply.unitUpS = 86401;  // since when that minimum counts (#624)
  size_t n = buildTelemetryPayload(buf, sizeof(buf), 25048UL, 12, -61L, 2, 3600UL, true, 3900, 6UL, false, 0, &supply);
  TEST_ASSERT_TRUE(n > 0 && n < sizeof(buf));
  assert_contains(buf, "\"vccMin\":3900,\"vccMinUnit\":39,\"vccMinBoard\":\"split-flap-row1\",\"vccUnder\":2,\"vccMinUnitUp\":86401,\"reboots\":6");
  // Without a vccMin there is no weakest unit either.
  n = buildTelemetryPayload(buf, sizeof(buf), 25048UL, 12, -61L, 2, 3600UL, true, 0, 6UL, false, 0, &supply);
  TEST_ASSERT_NULL(strstr(buf, "vccMinUnit"));
}

// The widest packet fits the buffer the service gives it: a packet that does
// not fit is not published at all.
static void test_telemetry_at_its_widest_fits_its_buffer() {
  char buf[MQTT_TELEMETRY_MAX];
  MqttSupplyFacts supply;
  supply.unit = 126;
  supply.board = "split-flap-row10-c10";
  supply.under = 99;
  supply.unitUpS = 0xFFFFFFFFUL;
  const size_t n = buildTelemetryPayload(buf, sizeof(buf), 0xFFFFFFFFUL, 100, -100L, 65535,
                                         0xFFFFFFFFUL, true, 65535, 0xFFFFFFFFUL, true, 65535,
                                         &supply);
  TEST_ASSERT_TRUE(n > 0 && n < sizeof(buf));
  TEST_ASSERT_EQUAL_UINT32(strlen(buf), (uint32_t)n);
  TEST_ASSERT_EQUAL('}', buf[n - 1]);
}

// #619: flips, a rising counter.
static void test_discovery_flips_and_supply_sensors() {
  char tbuf[96];
  char pbuf[512];
  buildDiscoveryTopic(tbuf, sizeof(tbuf), DISCOVERY_FLIPS, "flappy");
  TEST_ASSERT_EQUAL_STRING("homeassistant/sensor/flappy_flips/config", tbuf);
  buildDiscoveryPayload(pbuf, sizeof(pbuf), DISCOVERY_FLIPS, "flappy", "abc1234");
  assert_contains(pbuf, "\"stat_t\":\"splitflap/flappy/flips\"");
  assert_contains(pbuf, "\"stat_cla\":\"total_increasing\"");
  buildDiscoveryPayload(pbuf, sizeof(pbuf), DISCOVERY_VCC_MIN_UNIT, "flappy", "abc1234");
  assert_contains(pbuf, "{{ value_json.vccMinBoard }} #{{ value_json.vccMinUnit }}");
  buildDiscoveryPayload(pbuf, sizeof(pbuf), DISCOVERY_VCC_UNDER_FLOOR, "flappy", "abc1234");
  assert_contains(pbuf, "{{ value_json.vccUnder }}");
  assert_contains(pbuf, "\"stat_cla\":\"measurement\"");
  // #624: the button that restarts every unit, so the minimum starts over.
  buildDiscoveryTopic(tbuf, sizeof(tbuf), DISCOVERY_RESTART_UNITS, "flappy");
  TEST_ASSERT_EQUAL_STRING("homeassistant/button/flappy_restart_units/config", tbuf);
  buildDiscoveryPayload(pbuf, sizeof(pbuf), DISCOVERY_RESTART_UNITS, "flappy", "abc1234");
  assert_contains(pbuf, "\"cmd_t\":\"splitflap/flappy/restart_units/set\"");
  assert_contains(pbuf, "\"pl_prs\":\"PRESS\"");
  assert_contains(pbuf, "\"ent_cat\":\"diagnostic\"");
  TEST_ASSERT_NULL(strstr(pbuf, "dev_cla"));
}

static void test_telemetry_vccmin_omitted_when_zero() {
  // #366: no unit reports vitals -> vccMin 0 -> the key is dropped so the HA
  // sensor reads unavailable instead of a phantom 0 mV. Object still closes.
  char buf[160];
  size_t n = buildTelemetryPayload(buf, sizeof(buf), 25048UL, 12, -61L, 2, 3600UL, true, 0, 0UL, false, 0);
  TEST_ASSERT_NULL(strstr(buf, "vccMin"));
  // #368: unlike vccMin, 0 reboots is a real reading, not a sentinel — the
  // key is always present.
  assert_contains(buf, "\"reboots\":0");
  // #365: jam/stepsExcess are always emitted too — OFF/0 is a real
  // healthy reading, not a sentinel.
  assert_contains(buf, "\"jam\":\"OFF\"");
  assert_contains(buf, "\"stepsExcess\":0");
  TEST_ASSERT_EQUAL_CHAR('}', buf[strlen(buf) - 1]);
  TEST_ASSERT_EQUAL_UINT32(strlen(buf), (uint32_t)n);
}
static void test_telemetry_ntp_false_renders_zero() {
  char buf[160];
  buildTelemetryPayload(buf, sizeof(buf), 1UL, 0, 0L, 0, 0UL, false, 4700, 0UL, false, 0);
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

// #616: a held text has no deadline; a clear ends it and its priority.
static void test_held_notification_stays_until_cancelled() {
  MqttNotification n;
  notificationHold(n, String("LEAK"), 100);
  TEST_ASSERT_TRUE(notificationTick(n, 0UL));
  TEST_ASSERT_TRUE(notificationTick(n, 4000000000UL));
  TEST_ASSERT_TRUE(n.held);
  TEST_ASSERT_EQUAL(100, n.priority);
  notificationCancel(n);
  TEST_ASSERT_FALSE(notificationTick(n, 4000000000UL));
  TEST_ASSERT_FALSE(n.held);
  TEST_ASSERT_EQUAL(0, n.priority);
}

// #625: one comparison, no queue.
static void test_priority_admits_equal_or_higher_and_resets_with_the_text() {
  MqttNotification n;
  TEST_ASSERT_TRUE(notificationAdmits(n, 0));
  notificationStart(n, String("BRAND"), 600, 1000UL, 100);
  TEST_ASSERT_FALSE(notificationAdmits(n, 0));
  TEST_ASSERT_FALSE(notificationAdmits(n, 99));
  TEST_ASSERT_TRUE(notificationAdmits(n, 100));
  TEST_ASSERT_TRUE(notificationAdmits(n, MQTT_TEXT_PRIORITY_MAX));
  // The dwell ran out: anything goes again.
  TEST_ASSERT_FALSE(notificationTick(n, 1000UL + 600000UL));
  TEST_ASSERT_EQUAL(0, n.priority);
  TEST_ASSERT_TRUE(notificationAdmits(n, 0));
  // A text without a priority against one without: latest wins, as before.
  notificationStart(n, String("A"), 60, 0UL);
  TEST_ASSERT_TRUE(notificationAdmits(n, 0));
}

// #618: the Text entity's attributes say what a text may be made of.
static void test_text_attrs_carry_the_charset_and_the_width() {
  char buf[128];
  size_t n = buildTextAttrsJson(buf, sizeof(buf), " ABC$&#0-", 21);
  TEST_ASSERT_TRUE(n > 0 && n < sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("{\"charset\":\" ABC$&#0-\",\"width\":21}", buf);
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
  assert_contains(buf, "\"json_attr_t\":\"splitflap/flappy/text/attrs\"");  // #618
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
// #231: the wear-warning binary sensor alerts when any unit wears past the
// relative threshold; the median + flagged list ride a dedicated attrs topic
// (units/wear) so automations can name the offending unit.
static void test_discovery_unit_wear_topic_and_payload() {
  char buf[512];
  buildDiscoveryTopic(buf, sizeof(buf), DISCOVERY_UNIT_WEAR, "flappy");
  TEST_ASSERT_EQUAL_STRING("homeassistant/binary_sensor/flappy_unit_wear/config",
                           buf);

  buildDiscoveryPayload(buf, sizeof(buf), DISCOVERY_UNIT_WEAR, "flappy",
                        "abc1234");
  assert_contains(buf, "\"name\":\"Unit wear warning\"");
  assert_contains(buf, "\"stat_t\":\"splitflap/flappy/units_wear\"");
  assert_contains(buf, "\"json_attr_t\":\"splitflap/flappy/units/wear\"");
  assert_contains(buf, "\"uniq_id\":\"flappy_unit_wear\"");
  assert_contains(buf, "\"dev_cla\":\"problem\"");
  assert_contains(buf, "\"ent_cat\":\"diagnostic\"");
  assert_contains(buf, "\"pl_on\":\"ON\"");
}

static void test_discovery_vcc_min_topic_and_payload() {
  // #366: fleet-min supply Vcc diagnostic voltage sensor, telemetry-backed.
  char buf[512];
  buildDiscoveryTopic(buf, sizeof(buf), DISCOVERY_VCC_MIN, "flappy");
  TEST_ASSERT_EQUAL_STRING("homeassistant/sensor/flappy_vcc_min/config", buf);

  buildDiscoveryPayload(buf, sizeof(buf), DISCOVERY_VCC_MIN, "flappy", "abc1234");
  assert_contains(buf, "\"name\":\"Unit supply Vcc (min)\"");
  assert_contains(buf, "\"stat_t\":\"splitflap/flappy/telemetry\"");
  assert_contains(buf, "\"val_tpl\":\"{{ value_json.vccMin }}\"");
  assert_contains(buf, "\"uniq_id\":\"flappy_vcc_min\"");
  assert_contains(buf, "\"dev_cla\":\"voltage\"");
  assert_contains(buf, "\"unit_of_meas\":\"mV\"");
  assert_contains(buf, "\"ent_cat\":\"diagnostic\"");
}

// ---- Stage A4 fleet reboot-total (#368) ----
static void test_fleet_reboot_total_sums_valid_units() {
  // Sum of lifetime brownout+watchdog resets across every unit we hold a
  // valid CMD_GET_STATUS read for — mirrors unitFleetVccMin's statusValid
  // gate (UnitHealth.h). A unit we never read can't contribute.
  UnitFacts u[3];
  u[0].statusValid = true; u[0].status.lifetimeBrownoutCount = 3; u[0].status.lifetimeWatchdogCount = 1;
  u[1].statusValid = true; u[1].status.lifetimeBrownoutCount = 2; u[1].status.lifetimeWatchdogCount = 0;
  u[2].statusValid = false; u[2].status.lifetimeBrownoutCount = 9; u[2].status.lifetimeWatchdogCount = 9;
  TEST_ASSERT_EQUAL_UINT32(6, unitFleetRebootTotal(u, 3));
}

static void test_discovery_reboot_total_topic_and_payload() {
  // #368: fleet reboot odometer diagnostic sensor, telemetry-backed.
  char buf[512];
  buildDiscoveryTopic(buf, sizeof(buf), DISCOVERY_REBOOT_TOTAL, "flappy");
  TEST_ASSERT_EQUAL_STRING("homeassistant/sensor/flappy_reboot_total/config", buf);

  buildDiscoveryPayload(buf, sizeof(buf), DISCOVERY_REBOOT_TOTAL, "flappy", "abc1234");
  assert_contains(buf, "\"name\":\"Unit reboots (fleet)\"");
  assert_contains(buf, "\"stat_t\":\"splitflap/flappy/telemetry\"");
  assert_contains(buf, "\"val_tpl\":\"{{ value_json.reboots }}\"");
  assert_contains(buf, "\"uniq_id\":\"flappy_reboot_total\"");
  assert_contains(buf, "\"ent_cat\":\"diagnostic\"");
}

// ---- Fleet jam/steps-excess (#365) ----
static void test_fleet_any_jam_true_when_valid_unit_stalled() {
  UnitFacts u[3];
  u[0].extDiagValid = true;  u[0].extDiag.statusBits = 0;
  u[1].extDiagValid = true;  u[1].extDiag.statusBits = EXT_DIAG_STATUS_STALL;
  u[2].extDiagValid = false;
  TEST_ASSERT_TRUE(unitFleetAnyJam(u, 3));
}
static void test_fleet_any_jam_false_when_only_invalid_unit_stalled() {
  // A stall bit on a unit whose ext-diag read failed this epoch must not
  // count — extDiagValid gates every fleet ext-diag consumer.
  UnitFacts u[2];
  u[0].extDiagValid = false; u[0].extDiag.statusBits = EXT_DIAG_STATUS_STALL;
  u[1].extDiagValid = true;  u[1].extDiag.statusBits = 0;
  TEST_ASSERT_FALSE(unitFleetAnyJam(u, 2));
}
static void test_fleet_max_excess_returns_max_over_valid_units() {
  UnitFacts u[3];
  u[0].extDiagValid = true;  u[0].extDiag.stepExcessMax = 12;
  u[1].extDiagValid = true;  u[1].extDiag.stepExcessMax = 340;
  u[2].extDiagValid = false; u[2].extDiag.stepExcessMax = 9000;  // must not win
  TEST_ASSERT_EQUAL_UINT16(340, unitFleetMaxExcess(u, 3));
}
static void test_fleet_max_excess_zero_when_none_valid() {
  UnitFacts u[2];
  u[0].extDiagValid = false; u[0].extDiag.stepExcessMax = 50;
  u[1].extDiagValid = false; u[1].extDiag.stepExcessMax = 99;
  TEST_ASSERT_EQUAL_UINT16(0, unitFleetMaxExcess(u, 2));
}

static void test_discovery_unit_jam_topic_and_payload() {
  // #365: fleet jam/stall problem sensor, telemetry-backed like
  // vccMin/reboots. Binary_sensor with the same pl_on/pl_off "ON"/"OFF"
  // convention as the existing DISCOVERY_UNIT_WEAR problem sensor.
  char buf[512];
  buildDiscoveryTopic(buf, sizeof(buf), DISCOVERY_UNIT_JAM, "flappy");
  TEST_ASSERT_EQUAL_STRING("homeassistant/binary_sensor/flappy_unit_jam/config", buf);

  buildDiscoveryPayload(buf, sizeof(buf), DISCOVERY_UNIT_JAM, "flappy", "abc1234");
  assert_contains(buf, "\"name\":\"Unit jam\"");
  assert_contains(buf, "\"stat_t\":\"splitflap/flappy/telemetry\"");
  assert_contains(buf, "\"val_tpl\":\"{{ value_json.jam }}\"");
  assert_contains(buf, "\"uniq_id\":\"flappy_unit_jam\"");
  assert_contains(buf, "\"dev_cla\":\"problem\"");
  assert_contains(buf, "\"ent_cat\":\"diagnostic\"");
  assert_contains(buf, "\"pl_on\":\"ON\",\"pl_off\":\"OFF\"");
}

static void test_discovery_steps_excess_topic_and_payload() {
  // #365: fleet worst-seen home-step excess, telemetry-backed
  // diagnostic sensor (unitFleetMaxExcess).
  char buf[512];
  buildDiscoveryTopic(buf, sizeof(buf), DISCOVERY_STEPS_EXCESS, "flappy");
  TEST_ASSERT_EQUAL_STRING("homeassistant/sensor/flappy_steps_excess/config", buf);

  buildDiscoveryPayload(buf, sizeof(buf), DISCOVERY_STEPS_EXCESS, "flappy", "abc1234");
  assert_contains(buf, "\"name\":\"Unit steps excess (max)\"");
  assert_contains(buf, "\"stat_t\":\"splitflap/flappy/telemetry\"");
  assert_contains(buf, "\"val_tpl\":\"{{ value_json.stepsExcess }}\"");
  assert_contains(buf, "\"uniq_id\":\"flappy_steps_excess\"");
  assert_contains(buf, "\"unit_of_meas\":\"steps\"");
  assert_contains(buf, "\"ent_cat\":\"diagnostic\"");
}

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
    assert_contains(pbuf, "\"cu\":\"http://split-flap-9a3c1f.local/\"");  // #617: the master's own name
  }
}

// #615/#616/#620: the message, clear and firmware entities.
static void test_discovery_notify_clear_and_update_entities() {
  char tbuf[96];
  char pbuf[512];
  buildDiscoveryTopic(tbuf, sizeof(tbuf), DISCOVERY_NOTIFY, "flappy");
  TEST_ASSERT_EQUAL_STRING("homeassistant/notify/flappy/config", tbuf);
  buildDiscoveryPayload(pbuf, sizeof(pbuf), DISCOVERY_NOTIFY, "flappy", "abc1234");
  assert_contains(pbuf, "\"cmd_t\":\"splitflap/flappy/text/set\"");
  assert_contains(pbuf, "\"uniq_id\":\"flappy_notify\"");
  buildDiscoveryTopic(tbuf, sizeof(tbuf), DISCOVERY_TEXT_CLEAR, "flappy");
  TEST_ASSERT_EQUAL_STRING("homeassistant/button/flappy_text_clear/config", tbuf);
  buildDiscoveryPayload(pbuf, sizeof(pbuf), DISCOVERY_TEXT_CLEAR, "flappy", "abc1234");
  assert_contains(pbuf, "\"cmd_t\":\"splitflap/flappy/text/clear\"");
  assert_contains(pbuf, "\"pl_prs\":\"PRESS\"");
  buildDiscoveryTopic(tbuf, sizeof(tbuf), DISCOVERY_FIRMWARE_UPDATE, "flappy");
  TEST_ASSERT_EQUAL_STRING("homeassistant/update/flappy_firmware/config", tbuf);
  buildDiscoveryPayload(pbuf, sizeof(pbuf), DISCOVERY_FIRMWARE_UPDATE, "flappy", "abc1234");
  assert_contains(pbuf, "\"stat_t\":\"splitflap/flappy/update\"");
  assert_contains(pbuf, "\"cmd_t\":\"splitflap/flappy/update/set\"");
  assert_contains(pbuf, "\"pl_inst\":\"INSTALL\"");
  assert_contains(pbuf, "\"dev_cla\":\"firmware\"");
  // The restart button is a diagnostic (#620).
  buildDiscoveryPayload(pbuf, sizeof(pbuf), DISCOVERY_RESTART, "flappy", "abc1234");
  assert_contains(pbuf, "\"ent_cat\":\"diagnostic\"");
}

static void test_update_state_names_the_release_or_says_up_to_date() {
  char buf[256];
  size_t n = buildUpdateStateJson(buf, sizeof(buf), "abc1234", "def5678", "v2026.10.14",
                                  "https://x/tag/", false);
  TEST_ASSERT_TRUE(n > 0 && n < sizeof(buf));
  TEST_ASSERT_EQUAL_STRING(
      "{\"installed_version\":\"abc1234\",\"latest_version\":\"def5678\",\"in_progress\":false,"
      "\"title\":\"Split-Flap v2026.10.14\",\"release_url\":\"https://x/tag/v2026.10.14\"}",
      buf);
  // Nothing found yet: the running rev is the latest, so HA says up to date.
  n = buildUpdateStateJson(buf, sizeof(buf), "abc1234", "", "", "https://x/tag/", true);
  TEST_ASSERT_EQUAL_STRING(
      "{\"installed_version\":\"abc1234\",\"latest_version\":\"abc1234\",\"in_progress\":true,"
      "\"title\":\"Split-Flap firmware\"}",
      buf);
  TEST_ASSERT_TRUE(parseInstallCommand(" INSTALL "));
  TEST_ASSERT_FALSE(parseInstallCommand("install"));
  TEST_ASSERT_FALSE(parseInstallCommand(""));
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
  RUN_TEST(test_parse_unicode_escape_is_decoded);
  RUN_TEST(test_parse_reads_the_objects_own_members_only);
  RUN_TEST(test_parse_empty_text_is_valid);
  RUN_TEST(test_request_defaults_are_a_dwell_at_priority_zero);
  RUN_TEST(test_request_hold_by_flag_or_zero_dwell);
  RUN_TEST(test_request_priority_is_clamped_and_force_is_the_top);
  RUN_TEST(test_held_notification_stays_until_cancelled);
  RUN_TEST(test_priority_admits_equal_or_higher_and_resets_with_the_text);
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
  RUN_TEST(test_notification_end_is_a_clock_time_once_the_clock_is_set);
  RUN_TEST(test_mqttTopic_builds_expected_paths);
  RUN_TEST(test_telemetry_payload_exact_shape);
  RUN_TEST(test_telemetry_vccmin_omitted_when_zero);
  RUN_TEST(test_telemetry_supply_facts_follow_vccmin);
  RUN_TEST(test_telemetry_at_its_widest_fits_its_buffer);
  RUN_TEST(test_discovery_flips_and_supply_sensors);
  RUN_TEST(test_telemetry_ntp_false_renders_zero);
  RUN_TEST(test_mode_command_accepts_exact_options);
  RUN_TEST(test_mode_command_trims_whitespace);
  RUN_TEST(test_mode_command_rejects_case_variants_and_garbage);
  RUN_TEST(test_notification_cancel_mid_dwell);
  RUN_TEST(test_text_attrs_carry_the_charset_and_the_width);
  RUN_TEST(test_discovery_topics_per_entity);
  RUN_TEST(test_discovery_text_payload_fragments);
  RUN_TEST(test_discovery_sensor_payload_fragments);
  RUN_TEST(test_discovery_mode_select_topic_and_payload);
  RUN_TEST(test_discovery_new_topics);
  RUN_TEST(test_discovery_telemetry_backed_fragments);
  RUN_TEST(test_discovery_own_topic_fragments);
  RUN_TEST(test_discovery_diagnostics_fragments);
  RUN_TEST(test_all_discovery_entities_wellformed);
  RUN_TEST(test_discovery_notify_clear_and_update_entities);
  RUN_TEST(test_update_state_names_the_release_or_says_up_to_date);
  RUN_TEST(test_speed_command_accepts_in_range);
  RUN_TEST(test_speed_command_rejects_out_of_range_and_garbage);
  RUN_TEST(test_alignment_command_accepts_options_rejects_rest);
  RUN_TEST(test_restart_command_only_fires_on_press);
  RUN_TEST(test_discovery_control_topics_and_payloads);
  RUN_TEST(test_discovery_units_faulty_topic_and_payload);
  RUN_TEST(test_discovery_unit_wear_topic_and_payload);
  RUN_TEST(test_discovery_vcc_min_topic_and_payload);
  RUN_TEST(test_fleet_reboot_total_sums_valid_units);
  RUN_TEST(test_discovery_reboot_total_topic_and_payload);
  RUN_TEST(test_fleet_any_jam_true_when_valid_unit_stalled);
  RUN_TEST(test_fleet_any_jam_false_when_only_invalid_unit_stalled);
  RUN_TEST(test_fleet_max_excess_returns_max_over_valid_units);
  RUN_TEST(test_fleet_max_excess_zero_when_none_valid);
  RUN_TEST(test_discovery_unit_jam_topic_and_payload);
  RUN_TEST(test_discovery_steps_excess_topic_and_payload);
  return UNITY_END();
}
