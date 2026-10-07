// Native tests for WallSettingsJson.h: the settings as the operator API
// reads and writes them.
#include <ArduinoFake.h>
#include <unity.h>

#include <string>

#include "WallSettingsJson.h"

void setUp() {}
void tearDown() {}

// The key that was refused, nullptr when the body was taken.
static const char* put(const char* json, PendingSettingsPost& post, bool board = false) {
  static WallSettingsRefused refused;
  JsonDocument doc;
  TEST_ASSERT_TRUE(deserializeJson(doc, json) == DeserializationError::Ok);
  post = PendingSettingsPost();
  const bool ok = board ? boardSettingsBuild(doc.as<JsonVariantConst>(), post, refused)
                        : wallSettingsBuild(doc.as<JsonVariantConst>(), post, refused);
  return ok ? nullptr : refused.key;
}

static MasterSettings live() {
  MasterSettings s;
  s.alignment = "center";
  s.flapSpeed = 80;
  s.deviceMode = "clock";
  s.timezonePosix = "CET-1CEST,M3.5.0,M10.5.0/3";
  s.mqttHost = "192.168.1.4";
  s.mqttPort = 1883;
  s.mqttUser = "splitflap";
  s.mqttPassword = "secret";
  s.unitCountOverride = 0;
  s.reflashOnBoot = true;
  return s;
}

static void test_a_part_of_the_settings_stages_only_that_part() {
  PendingSettingsPost post;
  TEST_ASSERT_NULL(put("{\"speed\":55,\"alignment\":\"left\"}", post));
  TEST_ASSERT_TRUE(post.flapSpeedProvided);
  TEST_ASSERT_EQUAL_STRING("55", post.flapSpeed.c_str());
  TEST_ASSERT_TRUE(post.alignmentProvided);
  TEST_ASSERT_FALSE(post.deviceModeProvided);
  TEST_ASSERT_FALSE(post.quietProvided);
  TEST_ASSERT_FALSE(post.mqttHostProvided);
  TEST_ASSERT_FALSE(post.inputTextProvided);
}

static void test_every_wall_setting_is_taken_with_its_own_type() {
  PendingSettingsPost post;
  TEST_ASSERT_NULL(put("{\"mode\":\"text\",\"quiet\":true,\"alignment\":\"right\",\"speed\":1,"
                       "\"timezone\":\"UTC0\",\"updateUnitsAtStart\":false,"
                       "\"mqtt\":{\"host\":\"broker.lan\",\"port\":1884,\"user\":\"u\","
                       "\"password\":\"p\",\"passwordSet\":true}}",
                       post));
  TEST_ASSERT_EQUAL_STRING("text", post.deviceMode.c_str());
  TEST_ASSERT_EQUAL_STRING("true", post.quiet.c_str());
  TEST_ASSERT_EQUAL_STRING("UTC0", post.timezone.c_str());
  TEST_ASSERT_EQUAL_STRING("false", post.reflashOnBoot.c_str());
  TEST_ASSERT_EQUAL_STRING("broker.lan", post.mqttHost.c_str());
  TEST_ASSERT_EQUAL_STRING("1884", post.mqttPort.c_str());
  TEST_ASSERT_TRUE(post.mqttPasswordProvided);
}

static void test_one_bad_value_refuses_the_whole_request_and_names_the_key() {
  PendingSettingsPost post;
  TEST_ASSERT_EQUAL_STRING("speed", put("{\"alignment\":\"left\",\"speed\":101}", post));
  TEST_ASSERT_FALSE(post.alignmentProvided);
  TEST_ASSERT_EQUAL_STRING("speed", put("{\"speed\":\"80\"}", post));
  TEST_ASSERT_EQUAL_STRING("quiet", put("{\"quiet\":1}", post));
  TEST_ASSERT_EQUAL_STRING("quiet", put("{\"quiet\":\"true\"}", post));
  TEST_ASSERT_EQUAL_STRING("mode", put("{\"mode\":\"party\"}", post));
  TEST_ASSERT_EQUAL_STRING("port", put("{\"mqtt\":{\"port\":70000}}", post));
  TEST_ASSERT_EQUAL_STRING("mqtt", put("{\"mqtt\":\"broker\"}", post));
  TEST_ASSERT_EQUAL_STRING("", put("[1]", post));
}

static void test_a_key_that_is_no_setting_is_refused_not_ignored() {
  PendingSettingsPost post;
  TEST_ASSERT_EQUAL_STRING("sped", put("{\"sped\":80}", post));
  // A board's settings are not the wall's, and the other way round.
  TEST_ASSERT_EQUAL_STRING("name", put("{\"name\":\"x\"}", post));
  TEST_ASSERT_EQUAL_STRING("speed", put("{\"speed\":80}", post, true));
  TEST_ASSERT_EQUAL_STRING("text", put("{\"text\":\"HI\"}", post));
  // A key longer than a refusal names is cut, not overrun.
  TEST_ASSERT_EQUAL_STRING("aVeryLongKeyThatIsNoSett",
                           put("{\"aVeryLongKeyThatIsNoSettingAtAll\":1}", post));
}

static void test_an_empty_password_keeps_the_stored_one() {
  PendingSettingsPost post;
  TEST_ASSERT_NULL(put("{\"mqtt\":{\"user\":\"u\",\"password\":\"\"}}", post));
  TEST_ASSERT_TRUE(post.mqttUserProvided);
  TEST_ASSERT_FALSE(post.mqttPasswordProvided);
}

static void test_a_boards_own_settings() {
  PendingSettingsPost post;
  TEST_ASSERT_NULL(put("{\"name\":\"Hall-Wall\",\"unitCount\":12}", post, true));
  TEST_ASSERT_EQUAL_STRING("hall-wall", post.deviceName.c_str());  // names are lower case
  TEST_ASSERT_EQUAL_STRING("12", post.unitCount.c_str());
  TEST_ASSERT_NULL(put("{\"name\":\"\"}", post, true));  // back to the chip's name
  TEST_ASSERT_TRUE(post.deviceNameProvided);
  TEST_ASSERT_EQUAL_STRING("unitCount", put("{\"unitCount\":17}", post, true));
  TEST_ASSERT_EQUAL_STRING("name", put("{\"name\":\"no spaces\"}", post, true));
}

static void test_what_is_read_can_be_written_back_and_the_password_never_leaves() {
  JsonDocument doc;
  wallSettingsWrite(doc.to<JsonObject>(), live());
  std::string text;
  serializeJson(doc, text);
  TEST_ASSERT_TRUE(text.find("secret") == std::string::npos);
  TEST_ASSERT_TRUE(doc["mqtt"]["passwordSet"].as<bool>());
  PendingSettingsPost post;
  TEST_ASSERT_NULL(put(text.c_str(), post));
  TEST_ASSERT_EQUAL_STRING("clock", post.deviceMode.c_str());
  TEST_ASSERT_EQUAL_STRING("80", post.flapSpeed.c_str());
  TEST_ASSERT_FALSE(post.mqttPasswordProvided);
  // Nothing in what was read asks for a restart when written back.
  TEST_ASSERT_FALSE(settingsPostNeedsReboot(post, live()));

  JsonDocument board;
  boardSettingsWrite(board.to<JsonObject>(), live());
  text.clear();
  serializeJson(board, text);
  TEST_ASSERT_EQUAL_STRING("{\"name\":\"\",\"unitCount\":0}", text.c_str());
  TEST_ASSERT_NULL(put(text.c_str(), post, true));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_a_part_of_the_settings_stages_only_that_part);
  RUN_TEST(test_every_wall_setting_is_taken_with_its_own_type);
  RUN_TEST(test_one_bad_value_refuses_the_whole_request_and_names_the_key);
  RUN_TEST(test_a_key_that_is_no_setting_is_refused_not_ignored);
  RUN_TEST(test_an_empty_password_keeps_the_stored_one);
  RUN_TEST(test_a_boards_own_settings);
  RUN_TEST(test_what_is_read_can_be_written_back_and_the_password_never_leaves);
  return UNITY_END();
}
