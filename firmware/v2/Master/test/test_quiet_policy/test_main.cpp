// Host-side tests for QuietPolicy.h (#227): what switches quiet on and off,
// what passes while it is on, and the Home Assistant entity.

#include <ArduinoFake.h>
#include <unity.h>

#include <cstring>

#include "../../QuietPolicy.h"

void setUp() {}
void tearDown() {}

static void test_command_accepts_the_switch_payloads_only() {
  bool q = false;
  TEST_ASSERT_TRUE(quietParseCommand("ON", q));
  TEST_ASSERT_TRUE(q);
  TEST_ASSERT_TRUE(quietParseCommand("OFF", q));
  TEST_ASSERT_FALSE(q);
  TEST_ASSERT_TRUE(quietParseCommand(" on \n", q));
  TEST_ASSERT_TRUE(q);
  TEST_ASSERT_TRUE(quietParseCommand("0", q));
  TEST_ASSERT_FALSE(q);
  TEST_ASSERT_TRUE(quietParseCommand("true", q));
  TEST_ASSERT_TRUE(q);
  // Not a command: the state is left as it was.
  TEST_ASSERT_FALSE(quietParseCommand("", q));
  TEST_ASSERT_FALSE(quietParseCommand("toggle", q));
  TEST_ASSERT_FALSE(quietParseCommand("ONN", q));
  TEST_ASSERT_TRUE(q);
}

static void test_only_a_literal_json_true_forces_a_text() {
  TEST_ASSERT_TRUE(quietTextForced("{\"text\":\"BRAND KEUKEN\",\"dwell\":600,\"force\":true}"));
  TEST_ASSERT_TRUE(quietTextForced("{\"force\": true, \"text\":\"X\"}"));
  TEST_ASSERT_TRUE(quietTextForced(" {\"text\":\"X\",\"force\":true } "));
  TEST_ASSERT_FALSE(quietTextForced("{\"text\":\"X\",\"dwell\":60}"));
  TEST_ASSERT_FALSE(quietTextForced("{\"text\":\"X\",\"force\":false}"));
  TEST_ASSERT_FALSE(quietTextForced("{\"text\":\"X\",\"force\":\"true\"}"));
  TEST_ASSERT_FALSE(quietTextForced("{\"text\":\"X\",\"force\":1}"));
  TEST_ASSERT_FALSE(quietTextForced("{\"text\":\"X\",\"force\":trueish}"));
  // Only the top-level key counts: not the word as a value, not a nested
  // key, not the text itself.
  TEST_ASSERT_FALSE(quietTextForced("{\"note\":\"force\",\"x\":true}"));
  TEST_ASSERT_FALSE(quietTextForced("{\"meta\":{\"force\":true},\"text\":\"x\"}"));
  TEST_ASSERT_FALSE(quietTextForced("{\"text\":\"\\\"force\\\":true\"}"));
  TEST_ASSERT_FALSE(quietTextForced("{\"text\":\"force\"}"));
  TEST_ASSERT_TRUE(quietTextForced("{\"a\":\"force\",\"b\":\"z\",\"force\":true}"));
  TEST_ASSERT_TRUE(quietTextForced("{\n  \"text\": \"X\",\n  \"force\":\n    true\n}"));
  TEST_ASSERT_TRUE(quietTextForced("{\"text\":\"a } b\",\"force\":true}"));
  // A plain-text payload cannot force, whatever it says.
  TEST_ASSERT_FALSE(quietTextForced("\"force\":true"));
  TEST_ASSERT_FALSE(quietTextForced("force true"));
  TEST_ASSERT_FALSE(quietTextForced(""));
}

static void test_quiet_drops_everything_but_a_forced_text() {
  TEST_ASSERT_FALSE(quietBlocksContent(false, false));
  TEST_ASSERT_FALSE(quietBlocksContent(false, true));
  TEST_ASSERT_TRUE(quietBlocksContent(true, false));
  TEST_ASSERT_FALSE(quietBlocksContent(true, true));
}

static void test_discovery_is_a_retained_command_switch() {
  char topic[96];
  size_t t = buildQuietDiscoveryTopic(topic, sizeof(topic), "flap");
  TEST_ASSERT_TRUE(t > 0 && t < sizeof(topic));
  TEST_ASSERT_EQUAL_STRING("homeassistant/switch/flap_quiet/config", topic);
  char buf[512];
  size_t n = buildQuietDiscovery(buf, sizeof(buf), "flap", "abc1234");
  TEST_ASSERT_TRUE(n > 0 && n < sizeof(buf));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"name\":\"Quiet\""));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"cmd_t\":\"splitflap/flap/quiet/set\""));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"stat_t\":\"splitflap/flap/quiet\""));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"avty_t\":\"splitflap/flap/availability\""));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"uniq_id\":\"flap_quiet\""));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"pl_on\":\"ON\",\"pl_off\":\"OFF\""));
  // The board is often offline for the change: the command must be retained.
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"ret\":true"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"ids\":[\"flap\"]"));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_command_accepts_the_switch_payloads_only);
  RUN_TEST(test_only_a_literal_json_true_forces_a_text);
  RUN_TEST(test_quiet_drops_everything_but_a_forced_text);
  RUN_TEST(test_discovery_is_a_retained_command_switch);
  return UNITY_END();
}
