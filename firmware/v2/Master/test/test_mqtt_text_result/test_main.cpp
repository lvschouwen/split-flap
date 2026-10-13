// Native tests for MqttTextResult.h (#622): what became of the last text.
#include <ArduinoFake.h>
#include <unity.h>

#include <cstring>

#include "../../MqttTextResult.h"

void setUp() {}
void tearDown() {}

static bool contains(const char* s, const char* needle) { return strstr(s, needle) != nullptr; }

static void test_every_kind_has_a_name_and_is_announced() {
  for (int k = 0; k < (int)MqttTextResultKind::Count; k++) {
    TEST_ASSERT_TRUE(strlen(mqttTextResultName((MqttTextResultKind)k)) > 0);
  }
  char buf[512];
  size_t n = buildTextResultDiscovery(buf, sizeof(buf), "flappy", "abc1234");
  TEST_ASSERT_TRUE(n > 0 && n < sizeof(buf));
  TEST_ASSERT_TRUE(contains(buf, "\"stat_t\":\"splitflap/flappy/text/result\""));
  TEST_ASSERT_TRUE(contains(buf, "\"event_types\":[\"shown\",\"held\",\"refused\",\"dropped_quiet\",\"dropped_busy\",\"cleared\"]"));
  TEST_ASSERT_TRUE(contains(buf, "\"uniq_id\":\"flappy_text_result\""));
  TEST_ASSERT_TRUE(contains(buf, "\"ids\":[\"flappy\"]"));
  TEST_ASSERT_EQUAL_CHAR('}', buf[n - 1]);
  char topic[96];
  buildTextResultDiscoveryTopic(topic, sizeof(topic), "flappy");
  TEST_ASSERT_EQUAL_STRING("homeassistant/event/flappy_text_result/config", topic);
}

static void test_the_result_carries_what_was_asked_and_the_sequence() {
  MqttTextResult r;
  r.kind = MqttTextResultKind::Held;
  r.priority = 100;
  r.hold = true;
  strcpy(r.text, "WATER \"LEK\"");
  String json = buildTextResultJson(r, 7, 21, 1);
  TEST_ASSERT_EQUAL_STRING(
      "{\"event_type\":\"held\",\"text\":\"WATER \\\"LEK\\\"\",\"priority\":100,\"hold\":true,"
      "\"dwell\":0,\"seq\":7,\"truncated\":false,\"unsupported\":1}",
      json.c_str());
}

static void test_truncated_means_more_letters_than_the_wall_has() {
  MqttTextResult r;
  r.kind = MqttTextResultKind::Shown;
  r.dwellSeconds = 60;
  strcpy(r.text, "ABCDE\nFGHIJ");  // 10 letters on two lines
  TEST_ASSERT_TRUE(contains(buildTextResultJson(r, 1, 10, 0).c_str(), "\"truncated\":false"));
  TEST_ASSERT_TRUE(contains(buildTextResultJson(r, 1, 9, 0).c_str(), "\"truncated\":true"));
  // An unknown width says nothing.
  TEST_ASSERT_TRUE(contains(buildTextResultJson(r, 1, 0, 0).c_str(), "\"truncated\":false"));
  TEST_ASSERT_TRUE(contains(buildTextResultJson(r, 1, 10, 0).c_str(), "\"dwell\":60"));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_every_kind_has_a_name_and_is_announced);
  RUN_TEST(test_the_result_carries_what_was_asked_and_the_sequence);
  RUN_TEST(test_truncated_means_more_letters_than_the_wall_has);
  return UNITY_END();
}
