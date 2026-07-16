// Host-side tests for the follower's wire-reply builders (#298): the join /
// ping reply JSON the S3 leader parses (#294 health keys + the #297
// additive plat/vitals block), the tiny /settings JSON the member ⚙ panel
// reads, /cluster/health, and the faultMask hex fragment.

#include <ArduinoFake.h>
#include <unity.h>

#include "../../FollowerCors.h"
#include "../../FollowerJson.h"

void setUp() {}
void tearDown() {}

static FollowerHealthFacts makeHealth() {
  FollowerHealthFacts h;
  h.width = 8;
  h.detected = 8;
  h.faulty = 2;
  h.faultMask = "05";
  h.wear = false;
  return h;
}

static FollowerVitals makeVitals() {
  FollowerVitals v;
  v.heapBytes = 28000;
  v.rssiDbm = -61;
  v.upSeconds = 1200;
  return v;
}

// --- fault mask -------------------------------------------------------------------

static void test_fault_mask_width_sets_nibble_count() {
  UnitFacts units[16];
  units[0].statusValid = true;
  units[0].status.flags = UNIT_FLAG_LAST_HOME_FAILED;
  units[2].statusValid = true;
  units[2].status.flags = UNIT_FLAG_HALL_NEVER;
  char buf[16];
  TEST_ASSERT_EQUAL_UINT(2, followerFaultMaskHex(units, 8, buf, sizeof(buf)));
  TEST_ASSERT_EQUAL_STRING("05", buf);
  TEST_ASSERT_EQUAL_UINT(4, followerFaultMaskHex(units, 16, buf, sizeof(buf)));
  TEST_ASSERT_EQUAL_STRING("0005", buf);
}

static void test_fault_mask_zero_width_is_empty() {
  char buf[16];
  TEST_ASSERT_EQUAL_UINT(0, followerFaultMaskHex(nullptr, 0, buf, sizeof(buf)));
  TEST_ASSERT_EQUAL_STRING("", buf);
}

// --- join reply -------------------------------------------------------------------

static void test_join_reply_carries_identity_health_plat_vitals() {
  String out = followerJoinReplyJson("esp01-row", "abc1234", makeHealth(),
                                     makeVitals());
  TEST_ASSERT_TRUE(out.indexOf("\"name\":\"esp01-row\"") >= 0);
  TEST_ASSERT_TRUE(out.indexOf("\"rev\":\"abc1234\"") >= 0);
  TEST_ASSERT_TRUE(out.indexOf("\"width\":8") >= 0);
  TEST_ASSERT_TRUE(out.indexOf("\"detected\":8") >= 0);
  TEST_ASSERT_TRUE(out.indexOf("\"faulty\":2") >= 0);
  TEST_ASSERT_TRUE(out.indexOf("\"faultMask\":\"05\"") >= 0);
  TEST_ASSERT_TRUE(out.indexOf("\"wear\":false") >= 0);
  TEST_ASSERT_TRUE(out.indexOf("\"plat\":\"esp01\"") >= 0);
  TEST_ASSERT_TRUE(out.indexOf("\"heap\":28000") >= 0);
  TEST_ASSERT_TRUE(out.indexOf("\"rssi\":-61") >= 0);
  TEST_ASSERT_TRUE(out.indexOf("\"up\":1200") >= 0);
  TEST_ASSERT_TRUE(out.indexOf("\"protocol\":1") >= 0);
}

// --- ping reply -------------------------------------------------------------------

static void test_ping_reply_carries_state_and_health_and_plat() {
  String out = followerPingReplyJson("clustered", 7, 3, makeHealth(),
                                     makeVitals(), "abc1234");
  TEST_ASSERT_TRUE(out.startsWith("{\"state\":\"clustered\",\"epoch\":7,\"seq\":3"));
  TEST_ASSERT_TRUE(out.indexOf("\"faultMask\":\"05\"") >= 0);
  TEST_ASSERT_TRUE(out.indexOf("\"rev\":\"abc1234\"") >= 0);
  TEST_ASSERT_TRUE(out.indexOf("\"plat\":\"esp01\"") >= 0);
  TEST_ASSERT_TRUE(out.indexOf("\"heap\":28000") >= 0);
}

// --- /settings --------------------------------------------------------------------

static void test_settings_json_shape() {
  String out = followerSettingsJson("split-flap-c8a746", "abc1234", 8,
                                    "clustered", "wall-leader",
                                    "192.168.15.22", 2, makeVitals());
  TEST_ASSERT_TRUE(out.indexOf("\"deviceName\":\"split-flap-c8a746\"") >= 0);
  TEST_ASSERT_TRUE(out.indexOf("\"effectiveDeviceName\":\"split-flap-c8a746\"") >= 0);
  TEST_ASSERT_TRUE(out.indexOf("\"version\":\"abc1234\"") >= 0);
  TEST_ASSERT_TRUE(out.indexOf("\"plat\":\"esp01\"") >= 0);
  TEST_ASSERT_TRUE(out.indexOf("\"width\":8") >= 0);
  TEST_ASSERT_TRUE(out.indexOf("\"clusterState\":\"clustered\"") >= 0);
  TEST_ASSERT_TRUE(out.indexOf("\"clusterLeaderName\":\"wall-leader\"") >= 0);
  TEST_ASSERT_TRUE(out.indexOf("\"clusterLeaderHost\":\"192.168.15.22\"") >= 0);
  TEST_ASSERT_TRUE(out.indexOf("\"clusterRow\":2") >= 0);
  TEST_ASSERT_TRUE(out.indexOf("\"heap\":28000") >= 0);
  TEST_ASSERT_TRUE(out.indexOf("\"rssi\":-61") >= 0);
  TEST_ASSERT_TRUE(out.indexOf("\"up\":1200") >= 0);
}

// --- /cluster/health ----------------------------------------------------------------

static void test_cluster_health_json_shape() {
  FollowerClusterDiag d;
  d.msSinceRender = 4200;
  d.secsUntilBlank = 95;
  d.i2cTx = 1000;
  d.i2cErr = 2;
  d.minHeap = 21000;
  d.sntpSynced = true;
  String out = followerClusterHealthJson("grace", "wall-leader",
                                         "192.168.15.22", 2, 7, 3,
                                         "ROW THREE       ", "abc1234", 8, 8,
                                         0, d);
  TEST_ASSERT_TRUE(out.indexOf("\"state\":\"grace\"") >= 0);
  TEST_ASSERT_TRUE(out.indexOf("\"leaderName\":\"wall-leader\"") >= 0);
  TEST_ASSERT_TRUE(out.indexOf("\"leaderHost\":\"192.168.15.22\"") >= 0);
  TEST_ASSERT_TRUE(out.indexOf("\"row\":2") >= 0);
  TEST_ASSERT_TRUE(out.indexOf("\"epoch\":7") >= 0);
  TEST_ASSERT_TRUE(out.indexOf("\"seq\":3") >= 0);
  TEST_ASSERT_TRUE(out.indexOf("\"segment\":\"ROW THREE       \"") >= 0);
  TEST_ASSERT_TRUE(out.indexOf("\"rev\":\"abc1234\"") >= 0);
  TEST_ASSERT_TRUE(out.indexOf("\"width\":8") >= 0);
  // Diagnostics (#306).
  TEST_ASSERT_TRUE(out.indexOf("\"msSinceRender\":4200") >= 0);
  TEST_ASSERT_TRUE(out.indexOf("\"secsUntilBlank\":95") >= 0);
  TEST_ASSERT_TRUE(out.indexOf("\"i2cTx\":1000") >= 0);
  TEST_ASSERT_TRUE(out.indexOf("\"i2cErr\":2") >= 0);
  TEST_ASSERT_TRUE(out.indexOf("\"minHeap\":21000") >= 0);
  TEST_ASSERT_TRUE(out.indexOf("\"sntpSynced\":true") >= 0);
}

// --- string escaping ----------------------------------------------------------------

static void test_wire_strings_are_escaped() {
  // Leader name/host come off an unauthenticated LAN POST — a quote must
  // not break the JSON (same rule as every other builder in the fleet).
  FollowerClusterDiag d;
  String out = followerClusterHealthJson("clustered", "evil\"name",
                                         "10.0.0.9", 0, 1, 1, "X", "r", 1, 1,
                                         0, d);
  TEST_ASSERT_TRUE(out.indexOf("evil\\\"name") >= 0);
}

// --- CORS gates (#294 copies) ---------------------------------------------------------

static void test_cors_origin_gate_lan_only() {
  TEST_ASSERT_TRUE(followerCorsOriginAllowed("http://192.168.15.90"));
  TEST_ASSERT_TRUE(followerCorsOriginAllowed("http://10.1.2.3:8080"));
  TEST_ASSERT_TRUE(followerCorsOriginAllowed("http://leader.local"));
  TEST_ASSERT_TRUE(followerCorsOriginAllowed("http://localhost:8000"));
  TEST_ASSERT_FALSE(followerCorsOriginAllowed("https://192.168.15.90"));
  TEST_ASSERT_FALSE(followerCorsOriginAllowed("http://8.8.8.8"));
  TEST_ASSERT_FALSE(followerCorsOriginAllowed("http://evil.example.com"));
}

static void test_cors_path_gate_matches_served_surface() {
  // The wall-pane management surface; /cluster/* stays closed (leader wire,
  // not a browser surface). #304 opens /reflash-units (board-level unit
  // reflash from the wall). /firmware/* stays CLOSED, in lockstep with the
  // S3's clusterCorsPathAllowed copy: the ESP-01's firmware is pushed by the
  // S3 relay (stored image, server-to-server), not a browser POST.
  TEST_ASSERT_TRUE(followerCorsPathAllowed("/settings"));
  TEST_ASSERT_TRUE(followerCorsPathAllowed("/units/health"));
  TEST_ASSERT_TRUE(followerCorsPathAllowed("/units/health/refresh"));
  TEST_ASSERT_TRUE(followerCorsPathAllowed("/unit/jog"));
  TEST_ASSERT_TRUE(followerCorsPathAllowed("/unit/op-result"));
  TEST_ASSERT_TRUE(followerCorsPathAllowed("/reboot"));
  TEST_ASSERT_TRUE(followerCorsPathAllowed("/reflash-units"));
  TEST_ASSERT_FALSE(followerCorsPathAllowed("/firmware/master"));
  TEST_ASSERT_FALSE(followerCorsPathAllowed("/cluster/join"));
}

static void test_csrf_gate_matches_master() {
  // #313: a mutating POST with a public/https origin is cross-site forgery.
  TEST_ASSERT_TRUE(followerCsrfRejectPost(true, true, "http://evil.example.com"));
  TEST_ASSERT_TRUE(followerCsrfRejectPost(true, true, "https://192.168.15.90"));
  // The board's own LAN UI and server-to-server (no Origin) both pass; GETs
  // are never blocked.
  TEST_ASSERT_FALSE(followerCsrfRejectPost(true, true, "http://192.168.15.90"));
  TEST_ASSERT_FALSE(followerCsrfRejectPost(true, false, ""));
  TEST_ASSERT_FALSE(followerCsrfRejectPost(false, true, "http://evil.example.com"));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_cors_origin_gate_lan_only);
  RUN_TEST(test_cors_path_gate_matches_served_surface);
  RUN_TEST(test_csrf_gate_matches_master);
  RUN_TEST(test_fault_mask_width_sets_nibble_count);
  RUN_TEST(test_fault_mask_zero_width_is_empty);
  RUN_TEST(test_join_reply_carries_identity_health_plat_vitals);
  RUN_TEST(test_ping_reply_carries_state_and_health_and_plat);
  RUN_TEST(test_settings_json_shape);
  RUN_TEST(test_cluster_health_json_shape);
  RUN_TEST(test_wire_strings_are_escaped);
  return UNITY_END();
}
