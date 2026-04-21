// Host-side tests for MqttTopicDecoder.h.
//
// Exercises the pure parser + payload validator that live in the header.
// Deliberately has no dependency on AsyncMqttClient or WiFi — these are the
// two cheap native-env targets we could carve out of the MQTT feature, and
// they cover the dispatch path that actually acts on external input. Issue #50.

#include <ArduinoFake.h>
#include <unity.h>

#include "../../MqttTopicDecoder.h"

static const String kPrefix = String("split-flap/split-flap/cmd/");

void setUp() {
  ArduinoFakeReset();
}
void tearDown() {}

// --- cmd topic decoder ---------------------------------------------------

static void test_decode_stop() {
  MqttCmdDecoded d = mqttDecodeCmdTopic(kPrefix + "stop", kPrefix);
  TEST_ASSERT_EQUAL_UINT8(MqttCmdDecoded::STOP, d.kind);
  TEST_ASSERT_EQUAL_INT(0, d.unitAddress);
}

static void test_decode_home_all() {
  MqttCmdDecoded d = mqttDecodeCmdTopic(kPrefix + "home_all", kPrefix);
  TEST_ASSERT_EQUAL_UINT8(MqttCmdDecoded::HOME_ALL, d.kind);
}

static void test_decode_message() {
  MqttCmdDecoded d = mqttDecodeCmdTopic(kPrefix + "message", kPrefix);
  TEST_ASSERT_EQUAL_UINT8(MqttCmdDecoded::MESSAGE, d.kind);
}

static void test_decode_unit_home_valid() {
  MqttCmdDecoded d = mqttDecodeCmdTopic(kPrefix + "unit/5/home", kPrefix);
  TEST_ASSERT_EQUAL_UINT8(MqttCmdDecoded::UNIT_HOME, d.kind);
  TEST_ASSERT_EQUAL_INT(5, d.unitAddress);
}

static void test_decode_unit_reboot_valid() {
  MqttCmdDecoded d = mqttDecodeCmdTopic(kPrefix + "unit/42/reboot", kPrefix);
  TEST_ASSERT_EQUAL_UINT8(MqttCmdDecoded::UNIT_REBOOT, d.kind);
  TEST_ASSERT_EQUAL_INT(42, d.unitAddress);
}

static void test_decode_unit_home_at_i2c_boundaries() {
  //Lowest valid I2C 7-bit address in our system is 1 (address 0 is reserved
  //for the general call used by broadcastHome).
  MqttCmdDecoded d1 = mqttDecodeCmdTopic(kPrefix + "unit/1/home", kPrefix);
  TEST_ASSERT_EQUAL_UINT8(MqttCmdDecoded::UNIT_HOME, d1.kind);
  TEST_ASSERT_EQUAL_INT(1, d1.unitAddress);

  //Highest valid I2C 7-bit address is 127.
  MqttCmdDecoded d127 = mqttDecodeCmdTopic(kPrefix + "unit/127/home", kPrefix);
  TEST_ASSERT_EQUAL_UINT8(MqttCmdDecoded::UNIT_HOME, d127.kind);
  TEST_ASSERT_EQUAL_INT(127, d127.unitAddress);
}

static void test_decode_unit_addr_zero_rejected() {
  //Addr 0 is the I2C general-call address — broadcasting is legit but
  //targeting a single "unit 0" is not a real operation.
  MqttCmdDecoded d = mqttDecodeCmdTopic(kPrefix + "unit/0/home", kPrefix);
  TEST_ASSERT_EQUAL_UINT8(MqttCmdDecoded::NONE, d.kind);
}

static void test_decode_unit_addr_over_127_rejected() {
  MqttCmdDecoded d = mqttDecodeCmdTopic(kPrefix + "unit/128/home", kPrefix);
  TEST_ASSERT_EQUAL_UINT8(MqttCmdDecoded::NONE, d.kind);
}

static void test_decode_unit_addr_non_numeric_rejected() {
  //String::toInt() tolerates trailing garbage ("5abc" -> 5). The decoder
  //must reject that explicitly so a malformed topic can't trigger a real
  //operation by coincidence of numeric prefix.
  MqttCmdDecoded d = mqttDecodeCmdTopic(kPrefix + "unit/5abc/home", kPrefix);
  TEST_ASSERT_EQUAL_UINT8(MqttCmdDecoded::NONE, d.kind);
}

static void test_decode_unit_addr_negative_rejected() {
  //"-5" — starts with '-', not a digit. Decoder rejects on the char check
  //before toInt() gets a chance to produce -5.
  MqttCmdDecoded d = mqttDecodeCmdTopic(kPrefix + "unit/-5/home", kPrefix);
  TEST_ASSERT_EQUAL_UINT8(MqttCmdDecoded::NONE, d.kind);
}

static void test_decode_unit_empty_addr_rejected() {
  MqttCmdDecoded d = mqttDecodeCmdTopic(kPrefix + "unit//home", kPrefix);
  TEST_ASSERT_EQUAL_UINT8(MqttCmdDecoded::NONE, d.kind);
}

static void test_decode_unit_missing_action_rejected() {
  //"unit/5" — no action segment.
  MqttCmdDecoded d = mqttDecodeCmdTopic(kPrefix + "unit/5", kPrefix);
  TEST_ASSERT_EQUAL_UINT8(MqttCmdDecoded::NONE, d.kind);
}

static void test_decode_unit_empty_action_rejected() {
  MqttCmdDecoded d = mqttDecodeCmdTopic(kPrefix + "unit/5/", kPrefix);
  TEST_ASSERT_EQUAL_UINT8(MqttCmdDecoded::NONE, d.kind);
}

static void test_decode_unit_unknown_action_rejected() {
  MqttCmdDecoded d = mqttDecodeCmdTopic(kPrefix + "unit/5/dance", kPrefix);
  TEST_ASSERT_EQUAL_UINT8(MqttCmdDecoded::NONE, d.kind);
}

static void test_decode_unit_bootloader_deliberately_rejected() {
  //Destructive op — intentionally NOT routed via MQTT. Regression guard
  //in case someone adds it to the enum later without reading the rationale.
  MqttCmdDecoded d = mqttDecodeCmdTopic(kPrefix + "unit/5/bootloader", kPrefix);
  TEST_ASSERT_EQUAL_UINT8(MqttCmdDecoded::NONE, d.kind);
}

static void test_decode_wrong_prefix_rejected() {
  MqttCmdDecoded d = mqttDecodeCmdTopic(String("other/ns/cmd/stop"), kPrefix);
  TEST_ASSERT_EQUAL_UINT8(MqttCmdDecoded::NONE, d.kind);
}

static void test_decode_empty_prefix_rejects_everything() {
  MqttCmdDecoded d = mqttDecodeCmdTopic(kPrefix + "stop", String(""));
  TEST_ASSERT_EQUAL_UINT8(MqttCmdDecoded::NONE, d.kind);
}

static void test_decode_unknown_suffix_rejected() {
  MqttCmdDecoded d = mqttDecodeCmdTopic(kPrefix + "definitely_not_a_command", kPrefix);
  TEST_ASSERT_EQUAL_UINT8(MqttCmdDecoded::NONE, d.kind);
}

// --- message payload validator -------------------------------------------

static void test_validate_empty_payload_accepted() {
  TEST_ASSERT_TRUE(mqttValidateMessagePayload("", 0));
}

static void test_validate_short_payload_accepted() {
  const char* msg = "Hello";
  TEST_ASSERT_TRUE(mqttValidateMessagePayload(msg, 5));
}

static void test_validate_max_length_accepted() {
  char buf[MQTT_MESSAGE_MAX_LEN];
  for (size_t i = 0; i < sizeof(buf); i++) buf[i] = 'a';
  TEST_ASSERT_TRUE(mqttValidateMessagePayload(buf, MQTT_MESSAGE_MAX_LEN));
}

static void test_validate_over_max_length_rejected() {
  char buf[MQTT_MESSAGE_MAX_LEN + 1];
  for (size_t i = 0; i < sizeof(buf); i++) buf[i] = 'a';
  TEST_ASSERT_FALSE(mqttValidateMessagePayload(buf, sizeof(buf)));
}

static void test_validate_embedded_null_rejected() {
  const char payload[] = { 'H', 'i', 0x00, 'X' };
  TEST_ASSERT_FALSE(mqttValidateMessagePayload(payload, sizeof(payload)));
}

static void test_validate_newline_rejected() {
  //The display pipeline has its own \n handling via literal "\n" in the
  //HTML (see script.js/addNewline). An actual 0x0A in the payload is a
  //control char and shouldn't reach showText().
  const char payload[] = { 'H', 'i', '\n' };
  TEST_ASSERT_FALSE(mqttValidateMessagePayload(payload, sizeof(payload)));
}

static void test_validate_tab_accepted() {
  //Tab is the one C0 control we allow — it's effectively whitespace on
  //the display (unknown glyph -> blank) and some editors slip it in
  //unintentionally, so rejecting it outright is hostile.
  const char payload[] = { 'H', 'i', '\t' };
  TEST_ASSERT_TRUE(mqttValidateMessagePayload(payload, sizeof(payload)));
}

static void test_validate_del_rejected() {
  const char payload[] = { 'H', 'i', 0x7F };
  TEST_ASSERT_FALSE(mqttValidateMessagePayload(payload, sizeof(payload)));
}

static void test_validate_extended_ascii_accepted() {
  //Extended bytes (0x80+) are permissive: the display letters[] lookup
  //turns unknown glyphs into blanks, so they can't hurt. Tightening
  //here would break the web UI's $/&/# wire encoding for ä/ö/ü.
  const char payload[] = { (char)0xC3, (char)0xA4 };  //UTF-8 for ä
  TEST_ASSERT_TRUE(mqttValidateMessagePayload(payload, sizeof(payload)));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_decode_stop);
  RUN_TEST(test_decode_home_all);
  RUN_TEST(test_decode_message);
  RUN_TEST(test_decode_unit_home_valid);
  RUN_TEST(test_decode_unit_reboot_valid);
  RUN_TEST(test_decode_unit_home_at_i2c_boundaries);
  RUN_TEST(test_decode_unit_addr_zero_rejected);
  RUN_TEST(test_decode_unit_addr_over_127_rejected);
  RUN_TEST(test_decode_unit_addr_non_numeric_rejected);
  RUN_TEST(test_decode_unit_addr_negative_rejected);
  RUN_TEST(test_decode_unit_empty_addr_rejected);
  RUN_TEST(test_decode_unit_missing_action_rejected);
  RUN_TEST(test_decode_unit_empty_action_rejected);
  RUN_TEST(test_decode_unit_unknown_action_rejected);
  RUN_TEST(test_decode_unit_bootloader_deliberately_rejected);
  RUN_TEST(test_decode_wrong_prefix_rejected);
  RUN_TEST(test_decode_empty_prefix_rejects_everything);
  RUN_TEST(test_decode_unknown_suffix_rejected);
  RUN_TEST(test_validate_empty_payload_accepted);
  RUN_TEST(test_validate_short_payload_accepted);
  RUN_TEST(test_validate_max_length_accepted);
  RUN_TEST(test_validate_over_max_length_rejected);
  RUN_TEST(test_validate_embedded_null_rejected);
  RUN_TEST(test_validate_newline_rejected);
  RUN_TEST(test_validate_tab_accepted);
  RUN_TEST(test_validate_del_rejected);
  RUN_TEST(test_validate_extended_ascii_accepted);
  return UNITY_END();
}
