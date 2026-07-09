// Host-side unit tests for the pure mDNS broker-discovery logic in
// MdnsDiscovery.h (#129). No mDNS stack, no networking — only the
// candidate → prefill-suggestion rules and the /mqtt/discover JSON shape.

#include <ArduinoFake.h>
#include <unity.h>
#include "../../MdnsDiscovery.h"

using namespace fakeit;

void setUp() {
  ArduinoFakeReset();
}
void tearDown() {}

// ---- suggestedBrokerPort ----
static void test_mqtt_source_keeps_advertised_port() {
  MdnsBrokerCandidate c = {String("mosquitto"), String("192.168.1.20"), 8883, false};
  TEST_ASSERT_EQUAL_UINT16(8883, suggestedBrokerPort(c));
}

static void test_mqtt_source_without_port_defaults_to_1883() {
  MdnsBrokerCandidate c = {String("mosquitto"), String("192.168.1.20"), 0, false};
  TEST_ASSERT_EQUAL_UINT16(1883, suggestedBrokerPort(c));
}

static void test_home_assistant_source_always_suggests_1883() {
  // HA zeroconf advertises the web UI port (8123); the Mosquitto add-on on
  // the same host listens on 1883 — that's the useful suggestion.
  MdnsBrokerCandidate c = {String("homeassistant"), String("192.168.1.10"), 8123, true};
  TEST_ASSERT_EQUAL_UINT16(1883, suggestedBrokerPort(c));
}

// ---- preferredBrokerHost ----
static void test_prefers_ip_over_mdns_name() {
  // AsyncMqttClient resolves via DNS, not mDNS — a raw IP always works,
  // "<name>.local" only if the router happens to proxy it.
  MdnsBrokerCandidate c = {String("homeassistant"), String("192.168.1.10"), 8123, true};
  TEST_ASSERT_EQUAL_STRING("192.168.1.10", preferredBrokerHost(c).c_str());
}

static void test_falls_back_to_mdns_name_dot_local() {
  MdnsBrokerCandidate c = {String("mosquitto"), String(""), 1883, false};
  TEST_ASSERT_EQUAL_STRING("mosquitto.local", preferredBrokerHost(c).c_str());
}

// ---- normalizeMdnsHostname ----
static void test_normalize_strips_local_suffix_and_trailing_dot() {
  // LEAmDNS answer hostnames arrive in domain form ("host.local." or
  // "host.local"); candidates carry the bare label.
  TEST_ASSERT_EQUAL_STRING("homeassistant", normalizeMdnsHostname(String("homeassistant.local.")).c_str());
  TEST_ASSERT_EQUAL_STRING("homeassistant", normalizeMdnsHostname(String("homeassistant.local")).c_str());
}

static void test_normalize_leaves_bare_label_alone() {
  TEST_ASSERT_EQUAL_STRING("mosquitto", normalizeMdnsHostname(String("mosquitto")).c_str());
  TEST_ASSERT_EQUAL_STRING("", normalizeMdnsHostname(String("")).c_str());
}

// ---- buildDiscoverJson ----
static void test_no_candidates_yields_empty_array() {
  TEST_ASSERT_EQUAL_STRING("{\"status\":\"done\",\"candidates\":[]}",
                           buildDiscoverJson(nullptr, 0).c_str());
}

static void test_single_mqtt_candidate_shape() {
  MdnsBrokerCandidate c = {String("mosquitto"), String("192.168.1.20"), 1883, false};
  TEST_ASSERT_EQUAL_STRING(
    "{\"status\":\"done\",\"candidates\":["
    "{\"host\":\"192.168.1.20\",\"name\":\"mosquitto\",\"port\":1883,\"source\":\"mqtt\"}"
    "]}",
    buildDiscoverJson(&c, 1).c_str());
}

static void test_home_assistant_candidate_shape() {
  MdnsBrokerCandidate c = {String("homeassistant"), String("192.168.1.10"), 8123, true};
  TEST_ASSERT_EQUAL_STRING(
    "{\"status\":\"done\",\"candidates\":["
    "{\"host\":\"192.168.1.10\",\"name\":\"homeassistant\",\"port\":1883,\"source\":\"home-assistant\"}"
    "]}",
    buildDiscoverJson(&c, 1).c_str());
}

static void test_multiple_candidates_preserve_order() {
  MdnsBrokerCandidate cs[2] = {
    {String("brokerA"), String("192.168.1.20"), 1883, false},
    {String("brokerB"), String(""), 0, false},
  };
  TEST_ASSERT_EQUAL_STRING(
    "{\"status\":\"done\",\"candidates\":["
    "{\"host\":\"192.168.1.20\",\"name\":\"brokerA\",\"port\":1883,\"source\":\"mqtt\"},"
    "{\"host\":\"brokerB.local\",\"name\":\"brokerB\",\"port\":1883,\"source\":\"mqtt\"}"
    "]}",
    buildDiscoverJson(cs, 2).c_str());
}

static void test_hostname_with_quote_is_escaped() {
  // mDNS labels shouldn't contain quotes, but the answer comes off the wire —
  // never trust it into a JSON literal unescaped.
  MdnsBrokerCandidate c = {String("evil\"name"), String("192.168.1.9"), 1883, false};
  TEST_ASSERT_EQUAL_STRING(
    "{\"status\":\"done\",\"candidates\":["
    "{\"host\":\"192.168.1.9\",\"name\":\"evil\\\"name\",\"port\":1883,\"source\":\"mqtt\"}"
    "]}",
    buildDiscoverJson(&c, 1).c_str());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_mqtt_source_keeps_advertised_port);
  RUN_TEST(test_mqtt_source_without_port_defaults_to_1883);
  RUN_TEST(test_home_assistant_source_always_suggests_1883);
  RUN_TEST(test_prefers_ip_over_mdns_name);
  RUN_TEST(test_falls_back_to_mdns_name_dot_local);
  RUN_TEST(test_normalize_strips_local_suffix_and_trailing_dot);
  RUN_TEST(test_normalize_leaves_bare_label_alone);
  RUN_TEST(test_no_candidates_yields_empty_array);
  RUN_TEST(test_single_mqtt_candidate_shape);
  RUN_TEST(test_home_assistant_candidate_shape);
  RUN_TEST(test_multiple_candidates_preserve_order);
  RUN_TEST(test_hostname_with_quote_is_escaped);
  UNITY_END();
  return 0;
}
