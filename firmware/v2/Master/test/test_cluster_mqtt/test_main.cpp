// Host-side tests for ClusterMqtt.h (#277) — the leader's HA surfacing:
// cluster-degraded predicate, the per-member availability + rollout
// attributes JSON, the discovery config, and the wall text/state.

#include <unity.h>

#include <cstring>

#include "../../ClusterMqtt.h"

void setUp() {}
void tearDown() {}

// Leading 2-row status: self row 0 joined, one healthy remote follower.
static ClusterLeaderStatus makeHealthy() {
  ClusterLeaderStatus st;
  st.enabled = true;
  st.memberCount = 2;
  st.gridRows = 2;
  st.gridCapacity = 32;
  st.members[0].host = "";
  st.members[0].row = 0;
  st.members[0].width = 16;
  st.members[0].joined = true;
  st.members[1].host = "follower.local";
  st.members[1].row = 1;
  st.members[1].width = 16;
  st.members[1].joined = true;
  st.members[1].rev = "abc1234";
  st.rolloutPhase = "idle";
  return st;
}

// --- degraded predicate ----------------------------------------------------------

static void test_disabled_is_never_degraded() {
  ClusterLeaderStatus st = makeHealthy();
  st.enabled = false;
  st.members[1].joined = false;
  TEST_ASSERT_FALSE(clusterDegraded(st));
}

static void test_all_members_healthy_is_not_degraded() {
  TEST_ASSERT_FALSE(clusterDegraded(makeHealthy()));
}

static void test_unjoined_member_is_degraded() {
  ClusterLeaderStatus st = makeHealthy();
  st.members[1].joined = false;
  TEST_ASSERT_TRUE(clusterDegraded(st));
}

static void test_degraded_member_is_degraded() {
  ClusterLeaderStatus st = makeHealthy();
  st.members[1].degraded = true;
  TEST_ASSERT_TRUE(clusterDegraded(st));
}

static void test_update_blocked_member_is_degraded() {
  ClusterLeaderStatus st = makeHealthy();
  st.members[1].updateBlocked = true;
  TEST_ASSERT_TRUE(clusterDegraded(st));
}

static void test_image_verify_failure_is_degraded() {
  ClusterLeaderStatus st = makeHealthy();
  st.rolloutImageFailed = true;
  TEST_ASSERT_TRUE(clusterDegraded(st));
}

static void test_updating_alone_is_not_degraded() {
  // A rollout in progress is normal convergence, not a problem state.
  ClusterLeaderStatus st = makeHealthy();
  st.members[1].updating = true;
  st.rolloutPhase = "uploading";
  TEST_ASSERT_FALSE(clusterDegraded(st));
}

// --- attributes JSON -------------------------------------------------------------

static void test_attrs_json_shape() {
  ClusterLeaderStatus st = makeHealthy();
  st.rolloutHost = "follower.local";
  st.rolloutSent = 10;
  st.rolloutTotal = 100;
  String json = buildClusterAttrsJson(st);
  TEST_ASSERT_EQUAL_STRING(
      "{\"rows\":2,\"capacity\":32,\"members\":["
      "{\"host\":\"\",\"self\":true,\"row\":0,\"col\":0,\"width\":16,"
      "\"joined\":true,\"degraded\":false,\"updating\":false,"
      "\"updateBlocked\":false,\"rev\":\"\"},"
      "{\"host\":\"follower.local\",\"self\":false,\"row\":1,\"col\":0,"
      "\"width\":16,\"joined\":true,\"degraded\":false,\"updating\":false,"
      "\"updateBlocked\":false,\"rev\":\"abc1234\"}],"
      "\"rollout\":{\"phase\":\"idle\",\"host\":\"follower.local\","
      "\"sent\":10,\"total\":100},\"imageVerifyFailed\":false}",
      json.c_str());
}

static void test_attrs_json_escapes_network_strings() {
  // rev comes from a join reply body — hostile bytes must not break HA's
  // attribute JSON.
  ClusterLeaderStatus st = makeHealthy();
  st.members[1].rev = "a\"b\\c";
  String json = buildClusterAttrsJson(st);
  TEST_ASSERT_NOT_NULL(strstr(json.c_str(), "\"rev\":\"a\\\"b\\\\c\""));
}

// --- discovery config ------------------------------------------------------------

static void test_discovery_topic() {
  char buf[96];
  size_t n = buildClusterDegradedDiscoveryTopic(buf, sizeof(buf), "flap");
  TEST_ASSERT_TRUE(n > 0 && n < sizeof(buf));
  TEST_ASSERT_EQUAL_STRING(
      "homeassistant/binary_sensor/flap_cluster_degraded/config", buf);
}

static void test_discovery_payload_fields() {
  char buf[512];
  size_t n =
      buildClusterDegradedDiscovery(buf, sizeof(buf), "flap", "abc1234");
  TEST_ASSERT_TRUE(n > 0 && n < sizeof(buf));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"name\":\"Cluster degraded\""));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"stat_t\":\"splitflap/flap/cluster_degraded\""));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"json_attr_t\":\"splitflap/flap/cluster/attrs\""));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"avty_t\":\"splitflap/flap/availability\""));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"uniq_id\":\"flap_cluster_degraded\""));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"dev_cla\":\"problem\""));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"ent_cat\":\"diagnostic\""));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"pl_on\":\"ON\",\"pl_off\":\"OFF\""));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"ids\":[\"flap\"]"));  // shared device block
}

// --- wall text/state -------------------------------------------------------------

static void test_wall_state_joins_rows_with_newlines() {
  String rows[2] = {String("ROW ZERO"), String("ROW ONE")};
  TEST_ASSERT_EQUAL_STRING("ROW ZERO\nROW ONE",
                           clusterWallStateText(rows, 2).c_str());
}

static void test_wall_state_truncates_to_ha_state_limit() {
  String rows[2];
  for (int i = 0; i < 150; i++) {
    rows[0] += 'A';
    rows[1] += 'B';
  }
  String out = clusterWallStateText(rows, 2);
  TEST_ASSERT_EQUAL(255, (int)out.length());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_disabled_is_never_degraded);
  RUN_TEST(test_all_members_healthy_is_not_degraded);
  RUN_TEST(test_unjoined_member_is_degraded);
  RUN_TEST(test_degraded_member_is_degraded);
  RUN_TEST(test_update_blocked_member_is_degraded);
  RUN_TEST(test_image_verify_failure_is_degraded);
  RUN_TEST(test_updating_alone_is_not_degraded);
  RUN_TEST(test_attrs_json_shape);
  RUN_TEST(test_attrs_json_escapes_network_strings);
  RUN_TEST(test_discovery_topic);
  RUN_TEST(test_discovery_payload_fields);
  RUN_TEST(test_wall_state_joins_rows_with_newlines);
  RUN_TEST(test_wall_state_truncates_to_ha_state_limit);
  return UNITY_END();
}
