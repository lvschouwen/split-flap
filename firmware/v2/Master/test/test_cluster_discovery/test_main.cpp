// Host-side tests for the cluster discovery pure logic (#274) — TXT width
// parsing, self-filtering, and the /cluster/discover JSON contract the
// Cluster card consumes.

#include <unity.h>

#include "../../ClusterDiscovery.h"

void setUp() {}
void tearDown() {}

static ClusterDiscoveredBoard makeBoard(const char* name, const char* ip,
                                        const char* rev, int width) {
  ClusterDiscoveredBoard b;
  b.name = name;
  b.ip = ip;
  b.rev = rev;
  b.width = width;
  return b;
}

// --- TXT width parsing ---------------------------------------------------------

static void test_txt_width_parses_decimal() {
  TEST_ASSERT_EQUAL(16, clusterParseTxtWidth("16"));
  TEST_ASSERT_EQUAL(1, clusterParseTxtWidth("1"));
}

static void test_txt_width_rejects_garbage_to_zero() {
  TEST_ASSERT_EQUAL(0, clusterParseTxtWidth(""));
  TEST_ASSERT_EQUAL(0, clusterParseTxtWidth("abc"));
  TEST_ASSERT_EQUAL(0, clusterParseTxtWidth("16x"));
  TEST_ASSERT_EQUAL(0, clusterParseTxtWidth("-3"));
  TEST_ASSERT_EQUAL(0, clusterParseTxtWidth("999"));  // > 255: not a width
}

// --- host preference -----------------------------------------------------------

static void test_board_host_prefers_ip() {
  ClusterDiscoveredBoard b = makeBoard("split-flap-c8a746", "192.168.15.91",
                                       "0c5e1e0", 16);
  TEST_ASSERT_EQUAL_STRING("192.168.15.91",
                           clusterDiscoveredHost(b).c_str());
}

static void test_board_host_falls_back_to_mdns_name() {
  ClusterDiscoveredBoard b = makeBoard("split-flap-c8a746", "", "0c5e1e0", 16);
  TEST_ASSERT_EQUAL_STRING("split-flap-c8a746.local",
                           clusterDiscoveredHost(b).c_str());
}

// --- discover JSON -------------------------------------------------------------

static void test_discover_json_shape() {
  ClusterDiscoveredBoard boards[1] = {
      makeBoard("split-flap-c8a746", "192.168.15.91", "0c5e1e0", 16)};
  String json = buildClusterDiscoverJson(boards, 1, "split-flap-a47dee");
  TEST_ASSERT_EQUAL_STRING(
      "{\"status\":\"done\",\"boards\":[{\"name\":\"split-flap-c8a746\","
      "\"host\":\"192.168.15.91\",\"rev\":\"0c5e1e0\",\"width\":16}]}",
      json.c_str());
}

static void test_discover_json_empty() {
  String json = buildClusterDiscoverJson(nullptr, 0, "split-flap-a47dee");
  TEST_ASSERT_EQUAL_STRING("{\"status\":\"done\",\"boards\":[]}",
                           json.c_str());
}

static void test_discover_json_filters_self() {
  // The browsing master sees its own advertisement — never offer a board
  // to itself. mDNS names are case-insensitive.
  ClusterDiscoveredBoard boards[2] = {
      makeBoard("SPLIT-FLAP-A47DEE", "192.168.15.22", "0c5e1e0", 16),
      makeBoard("split-flap-c8a746", "192.168.15.91", "0c5e1e0", 16)};
  String json = buildClusterDiscoverJson(boards, 2, "split-flap-a47dee");
  TEST_ASSERT_TRUE(json.indexOf("c8a746") >= 0);
  TEST_ASSERT_TRUE(json.indexOf("a47dee") < 0);
  TEST_ASSERT_TRUE(json.indexOf("A47DEE") < 0);
}

static void test_discover_json_skips_nameless_answers() {
  ClusterDiscoveredBoard boards[2] = {
      makeBoard("", "192.168.15.50", "0c5e1e0", 16),
      makeBoard("split-flap-c8a746", "192.168.15.91", "0c5e1e0", 16)};
  String json = buildClusterDiscoverJson(boards, 2, "split-flap-a47dee");
  TEST_ASSERT_TRUE(json.indexOf("c8a746") >= 0);
  TEST_ASSERT_TRUE(json.indexOf("15.50") < 0);
}

static void test_discover_json_carries_plat_tag_only_when_advertised() {
  // #297: the ESP-01 follower advertises plat=esp01 in its TXT record; the
  // scan list tags it. S3 boards advertise no plat — the key stays absent
  // (the exact-shape test above pins that).
  ClusterDiscoveredBoard boards[1] = {
      makeBoard("esp01-row", "192.168.15.93", "abc1234", 8)};
  boards[0].plat = "esp01";
  String json = buildClusterDiscoverJson(boards, 1, "leader");
  TEST_ASSERT_TRUE(json.indexOf("\"plat\":\"esp01\"") >= 0);
}

static void test_discover_json_escapes_wire_strings() {
  // TXT values come off the wire — a quote must not break the JSON.
  ClusterDiscoveredBoard boards[1] = {
      makeBoard("evil\"name", "192.168.15.7", "rev\\x", 16)};
  String json = buildClusterDiscoverJson(boards, 1, "leader");
  TEST_ASSERT_TRUE(json.indexOf("evil\\\"name") >= 0);
  TEST_ASSERT_TRUE(json.indexOf("rev\\\\x") >= 0);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_txt_width_parses_decimal);
  RUN_TEST(test_txt_width_rejects_garbage_to_zero);
  RUN_TEST(test_board_host_prefers_ip);
  RUN_TEST(test_board_host_falls_back_to_mdns_name);
  RUN_TEST(test_discover_json_shape);
  RUN_TEST(test_discover_json_empty);
  RUN_TEST(test_discover_json_filters_self);
  RUN_TEST(test_discover_json_skips_nameless_answers);
  RUN_TEST(test_discover_json_carries_plat_tag_only_when_advertised);
  RUN_TEST(test_discover_json_escapes_wire_strings);
  return UNITY_END();
}
