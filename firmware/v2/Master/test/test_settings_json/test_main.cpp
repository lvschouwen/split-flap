// Host-side tests for the /settings JSON builder (#186).
// buildSettingsJson() is the v2 counterpart of v1's getCurrentSettingValues():
// same key set and value typing (one wire contract for /settings across both
// firmware generations), but pure — every field arrives via the struct, so
// the shape is testable without globals or hardware.

#include <ArduinoFake.h>
#include <unity.h>

#include "../../SettingsJson.h"

void setUp() {}
void tearDown() {}

static bool contains(const String& json, const char* needle) {
  return json.indexOf(needle) >= 0;
}

static void test_defaults_produce_v1_shaped_empty_document() {
  SettingsJsonFields f;
  f.unitsAmount = 3;
  String json = buildSettingsJson(f);

  TEST_ASSERT_EQUAL_CHAR('{', json[0]);
  TEST_ASSERT_EQUAL_CHAR('}', json[json.length() - 1]);
  TEST_ASSERT_TRUE(contains(json, "\"unitCount\":0"));
  TEST_ASSERT_TRUE(contains(json, "\"detectedUnitCount\":0"));
  TEST_ASSERT_TRUE(contains(json, "\"detectedUnitAddresses\":[]"));
  // Per-unit arrays always carry unitsAmount entries (v1: UNITS_AMOUNT),
  // defaulting to status 0 / empty version string per slot.
  TEST_ASSERT_TRUE(contains(json, "\"detectedUnitVersionStatus\":[0,0,0]"));
  TEST_ASSERT_TRUE(contains(json, "\"detectedUnitVersions\":[\"\",\"\",\"\"]"));
  TEST_ASSERT_TRUE(contains(json, "\"mqttPasswordSet\":false"));
  TEST_ASSERT_TRUE(contains(json, "\"mqttConnected\":false"));
  TEST_ASSERT_TRUE(contains(json, "\"otaReverted\":false"));
  TEST_ASSERT_TRUE(contains(json, "\"bootCounter\":0"));
  TEST_ASSERT_TRUE(contains(json, "\"recoveryMode\":false"));
  TEST_ASSERT_TRUE(contains(json, "\"flashConfigMismatch\":false"));
  TEST_ASSERT_TRUE(contains(json, "\"isInOtaMode\":false"));
  TEST_ASSERT_TRUE(contains(json, "\"wifiSettingsResettable\":false"));
}

static void test_flap_speed_is_string_typed_like_v1() {
  SettingsJsonFields f;
  f.flapSpeed = "80";
  String json = buildSettingsJson(f);
  TEST_ASSERT_TRUE(contains(json, "\"flapSpeed\":\"80\""));
}

static void test_populated_fields_appear_with_values() {
  SettingsJsonFields f;
  f.unitCount = 5;
  f.alignment = "center";
  f.deviceMode = "clock";
  f.timezonePosix = "UTC0";
  f.deviceName = "kitchen";
  f.effectiveDeviceName = "kitchen";
  f.mqttHost = "broker.local";
  f.mqttPort = "1883";
  f.mqttUser = "ha";
  f.mqttPasswordSet = true;
  f.version = "abc1234";
  f.sketchMd5 = "d41d8cd98f00b204e9800998ecf8427e";
  f.lastResetReason = "poweron";
  f.wifiSettingsResettable = false;
  String json = buildSettingsJson(f);

  TEST_ASSERT_TRUE(contains(json, "\"unitCount\":5"));
  TEST_ASSERT_TRUE(contains(json, "\"alignment\":\"center\""));
  TEST_ASSERT_TRUE(contains(json, "\"deviceMode\":\"clock\""));
  TEST_ASSERT_TRUE(contains(json, "\"timezonePosix\":\"UTC0\""));
  TEST_ASSERT_TRUE(contains(json, "\"deviceName\":\"kitchen\""));
  TEST_ASSERT_TRUE(contains(json, "\"effectiveDeviceName\":\"kitchen\""));
  TEST_ASSERT_TRUE(contains(json, "\"mqttHost\":\"broker.local\""));
  TEST_ASSERT_TRUE(contains(json, "\"mqttPort\":\"1883\""));
  TEST_ASSERT_TRUE(contains(json, "\"mqttUser\":\"ha\""));
  TEST_ASSERT_TRUE(contains(json, "\"mqttPasswordSet\":true"));
  TEST_ASSERT_TRUE(contains(json, "\"version\":\"abc1234\""));
  TEST_ASSERT_TRUE(contains(json, "\"sketchMd5\":\"d41d8cd98f00b204e9800998ecf8427e\""));
  TEST_ASSERT_TRUE(contains(json, "\"lastResetReason\":\"poweron\""));
}

static void test_password_value_never_appears() {
  // The builder has no password field at all — only the boolean. This test
  // pins the contract: no key named mqttPassword (mqttPasswordSet is fine).
  SettingsJsonFields f;
  f.mqttPasswordSet = true;
  String json = buildSettingsJson(f);
  TEST_ASSERT_TRUE(json.indexOf("\"mqttPassword\":") < 0);
}

static void test_string_values_are_json_escaped() {
  SettingsJsonFields f;
  f.lastWrittenText = "say \"hi\"\\\n";
  String json = buildSettingsJson(f);
  TEST_ASSERT_TRUE(contains(json, "\"lastWrittenText\":\"say \\\"hi\\\"\\\\\\n\""));
}

static void test_control_characters_use_unicode_escapes() {
  SettingsJsonFields f;
  f.lastWrittenText = String('\x01');
  String json = buildSettingsJson(f);
  TEST_ASSERT_TRUE(contains(json, "\"lastWrittenText\":\"\\u0001\""));
}

static void test_unit_arrays_carry_supplied_data() {
  const int addresses[] = {1, 2, 4};
  const int status[] = {1, 1, 0, 2};
  const String versions[] = {String("aa11"), String("bb22"), String(""), String("cc33")};
  SettingsJsonFields f;
  f.unitCount = 4;
  f.detectedUnitCount = 3;
  f.detectedUnitAddresses = addresses;
  f.unitsAmount = 4;
  f.detectedUnitVersionStatus = status;
  f.detectedUnitVersions = versions;
  String json = buildSettingsJson(f);

  TEST_ASSERT_TRUE(contains(json, "\"detectedUnitAddresses\":[1,2,4]"));
  TEST_ASSERT_TRUE(contains(json, "\"detectedUnitVersionStatus\":[1,1,0,2]"));
  TEST_ASSERT_TRUE(contains(json, "\"detectedUnitVersions\":[\"aa11\",\"bb22\",\"\",\"cc33\"]"));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_defaults_produce_v1_shaped_empty_document);
  RUN_TEST(test_flap_speed_is_string_typed_like_v1);
  RUN_TEST(test_populated_fields_appear_with_values);
  RUN_TEST(test_password_value_never_appears);
  RUN_TEST(test_string_values_are_json_escaped);
  RUN_TEST(test_control_characters_use_unicode_escapes);
  RUN_TEST(test_unit_arrays_carry_supplied_data);
  return UNITY_END();
}
