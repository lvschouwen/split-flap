// Native tests for WallActions.h: the actions of POST /api/v2/action that are
// done when they are accepted.
#include <ArduinoFake.h>
#include <unity.h>

#include "WallActions.h"

void setUp() {}
void tearDown() {}

static const char* build(const char* name, const char* argsJson, PendingSettingsPost& post) {
  JsonDocument doc;
  TEST_ASSERT_TRUE(deserializeJson(doc, argsJson) == DeserializationError::Ok);
  post = PendingSettingsPost();
  return wallActionBuild(name, doc.as<JsonVariantConst>(), post);
}

static void test_only_these_three_are_content_actions() {
  TEST_ASSERT_TRUE(wallActionIsContent("show"));
  TEST_ASSERT_TRUE(wallActionIsContent("mode"));
  TEST_ASSERT_TRUE(wallActionIsContent("quiet"));
  TEST_ASSERT_FALSE(wallActionIsContent("stop"));
  TEST_ASSERT_FALSE(wallActionIsContent("home"));
  TEST_ASSERT_FALSE(wallActionIsContent(""));
}

static void test_show_switches_to_text_and_carries_the_text() {
  PendingSettingsPost post;
  TEST_ASSERT_NULL(build("show", "{\"text\":\"HELLO\\nWORLD\"}", post));
  TEST_ASSERT_TRUE(post.inputTextProvided);
  TEST_ASSERT_EQUAL_STRING("HELLO\nWORLD", post.inputText.c_str());
  TEST_ASSERT_TRUE(post.deviceModeProvided);
  TEST_ASSERT_EQUAL_STRING("text", post.deviceMode.c_str());
  TEST_ASSERT_FALSE(post.transientTextProvided);
  // An empty text is a text: it blanks the wall.
  TEST_ASSERT_NULL(build("show", "{\"text\":\"\"}", post));
  TEST_ASSERT_TRUE(post.inputTextProvided);
}

static void test_show_for_a_time_leaves_the_mode_alone() {
  PendingSettingsPost post;
  TEST_ASSERT_NULL(build("show", "{\"text\":\"DINNER\",\"forS\":300}", post));
  TEST_ASSERT_TRUE(post.transientTextProvided);
  TEST_ASSERT_EQUAL_STRING("DINNER", post.transientText.c_str());
  TEST_ASSERT_TRUE(post.transientDwellProvided);
  TEST_ASSERT_EQUAL(300, post.transientDwell);
  TEST_ASSERT_FALSE(post.inputTextProvided);
  TEST_ASSERT_FALSE(post.deviceModeProvided);
  TEST_ASSERT_TRUE(settingsPostConsistent(post));
}

static void test_show_refuses_what_it_cannot_read() {
  PendingSettingsPost post;
  TEST_ASSERT_NOT_NULL(build("show", "{}", post));
  TEST_ASSERT_NOT_NULL(build("show", "{\"text\":5}", post));
  TEST_ASSERT_NOT_NULL(build("show", "{\"text\":\"A\",\"forS\":4}", post));
  TEST_ASSERT_NOT_NULL(build("show", "{\"text\":\"A\",\"forS\":3601}", post));
  TEST_ASSERT_NOT_NULL(build("show", "{\"text\":\"A\",\"forS\":\"60\"}", post));
  TEST_ASSERT_NOT_NULL(build("show", "{\"text\":\"A\",\"forS\":60.5}", post));
  TEST_ASSERT_NOT_NULL(build("show", "{\"text\":\"A\",\"speed\":3}", post));
  TEST_ASSERT_FALSE(post.pending);
  TEST_ASSERT_NULL(build("show", "{\"text\":\"A\",\"forS\":5}", post));
  TEST_ASSERT_NULL(build("show", "{\"text\":\"A\",\"forS\":3600}", post));
}

static void test_mode_is_clock_or_text() {
  PendingSettingsPost post;
  TEST_ASSERT_NULL(build("mode", "{\"mode\":\"clock\"}", post));
  TEST_ASSERT_TRUE(post.deviceModeProvided);
  TEST_ASSERT_EQUAL_STRING("clock", post.deviceMode.c_str());
  TEST_ASSERT_FALSE(post.inputTextProvided);
  TEST_ASSERT_NULL(build("mode", "{\"mode\":\"text\"}", post));
  TEST_ASSERT_NOT_NULL(build("mode", "{\"mode\":\"Clock\"}", post));
  TEST_ASSERT_NOT_NULL(build("mode", "{}", post));
  TEST_ASSERT_NOT_NULL(build("mode", "{\"mode\":\"clock\",\"on\":true}", post));
}

static void test_quiet_takes_a_real_boolean_only() {
  PendingSettingsPost post;
  TEST_ASSERT_NULL(build("quiet", "{\"on\":true}", post));
  TEST_ASSERT_TRUE(post.quietProvided);
  TEST_ASSERT_EQUAL_STRING("true", post.quiet.c_str());
  TEST_ASSERT_NULL(build("quiet", "{\"on\":false}", post));
  TEST_ASSERT_EQUAL_STRING("false", post.quiet.c_str());
  // A typo must not silence or wake the wall.
  TEST_ASSERT_NOT_NULL(build("quiet", "{\"on\":1}", post));
  TEST_ASSERT_NOT_NULL(build("quiet", "{\"on\":\"true\"}", post));
  TEST_ASSERT_NOT_NULL(build("quiet", "{}", post));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_only_these_three_are_content_actions);
  RUN_TEST(test_show_switches_to_text_and_carries_the_text);
  RUN_TEST(test_show_for_a_time_leaves_the_mode_alone);
  RUN_TEST(test_show_refuses_what_it_cannot_read);
  RUN_TEST(test_mode_is_clock_or_text);
  RUN_TEST(test_quiet_takes_a_real_boolean_only);
  return UNITY_END();
}
