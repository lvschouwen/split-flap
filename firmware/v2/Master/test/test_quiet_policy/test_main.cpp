// Host-side tests for QuietPolicy.h (#227): what switches quiet on and off,
// what passes while it is on, and the Home Assistant entity.

#include <ArduinoFake.h>
#include <unity.h>

#include <cstring>

#include "../../QuietPolicy.h"
#include "ClusterWireGuards.h"  // ClusterMemberAuth::macMatches

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

static void test_ping_param_reads_one_as_quiet() {
  TEST_ASSERT_TRUE(clusterQuietFromPing("1"));
  TEST_ASSERT_FALSE(clusterQuietFromPing("0"));
  TEST_ASSERT_FALSE(clusterQuietFromPing(""));
  TEST_ASSERT_FALSE(clusterQuietFromPing("true"));
  TEST_ASSERT_FALSE(clusterQuietFromPing("11"));
  TEST_ASSERT_FALSE(clusterQuietFromPing(nullptr));
  // What the leader sends parses back on the member.
  TEST_ASSERT_EQUAL_STRING("&quiet=1", clusterQuietPingSuffix(true));
  TEST_ASSERT_EQUAL_STRING("&quiet=0", clusterQuietPingSuffix(false));
}

// An unkeyed member has no authentication at all and takes the flag as sent.
// A keyed one takes it only with a valid mac; anything else changes nothing.
static void test_a_keyed_member_takes_the_flag_only_with_its_mac() {
  bool q = false;
  TEST_ASSERT_TRUE(clusterQuietAccept(true, "1", false, false, q));
  TEST_ASSERT_TRUE(q);
  TEST_ASSERT_TRUE(clusterQuietAccept(true, "0", false, false, q));
  TEST_ASSERT_FALSE(q);
  q = true;
  TEST_ASSERT_FALSE(clusterQuietAccept(true, "0", true, false, q));  // no/bad mac
  TEST_ASSERT_TRUE(q);                                               // kept
  TEST_ASSERT_TRUE(clusterQuietAccept(true, "0", true, true, q));
  TEST_ASSERT_FALSE(q);
  // A leader that predates the flag sends nothing: nothing changes.
  q = true;
  TEST_ASSERT_FALSE(clusterQuietAccept(false, "", false, false, q));
  TEST_ASSERT_FALSE(clusterQuietAccept(false, "", true, true, q));
  TEST_ASSERT_TRUE(q);
}

// The mac binds the value and the ping's timestamp: neither can be swapped.
static void test_the_quiet_mac_binds_value_and_timestamp() {
  ClusterMemberAuth member;
  for (int i = 0; i < CLUSTER_HMAC_KEY_LEN; i++) member.key[i] = (uint8_t)(i * 7 + 1);
  member.keyed = true;
  const uint64_t ts = 1759680000123ULL;
  String macOn = clusterHmacSign(member.key, clusterQuietMsg(ts, true));
  TEST_ASSERT_TRUE(member.macMatches(clusterQuietMsg(ts, true), macOn));
  TEST_ASSERT_FALSE(member.macMatches(clusterQuietMsg(ts, false), macOn));   // flipped value
  TEST_ASSERT_FALSE(member.macMatches(clusterQuietMsg(ts + 1, true), macOn)); // another ping
  ClusterMemberAuth other = member;
  other.key[0] ^= 0x01;
  TEST_ASSERT_FALSE(other.macMatches(clusterQuietMsg(ts, true), macOn));      // another key
  ClusterMemberAuth unkeyed;
  TEST_ASSERT_FALSE(unkeyed.macMatches(clusterQuietMsg(ts, true), macOn));
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
  RUN_TEST(test_ping_param_reads_one_as_quiet);
  RUN_TEST(test_a_keyed_member_takes_the_flag_only_with_its_mac);
  RUN_TEST(test_the_quiet_mac_binds_value_and_timestamp);
  RUN_TEST(test_discovery_is_a_retained_command_switch);
  return UNITY_END();
}
