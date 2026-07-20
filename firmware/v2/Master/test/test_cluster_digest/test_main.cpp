// Host-side tests for ClusterDigest.h (#294) — the single-pane-of-glass
// pure logic: per-row fault bitmap, ping-reply health fragment, the shared
// /cluster/status serializer, the ping-piggybacked digest, the promote
// table transform (#295) and the CORS origin validator.

#include <unity.h>

#include <cstring>

#include "../../ClusterDigest.h"

void setUp() {}
void tearDown() {}

// --- fault mask -------------------------------------------------------------------

static void makeUnits(UnitFacts* units, int n) {
  for (int i = 0; i < n; i++) units[i] = UnitFacts{};
}

static void setFaulty(UnitFacts& u) {
  u.statusValid = true;
  u.status.flags = UNIT_FLAG_LAST_HOME_FAILED;
}

static void setHealthy(UnitFacts& u) {
  u.statusValid = true;
  u.status = UnitStatus{};
}

static void test_fault_mask_all_healthy_is_zeroes() {
  UnitFacts units[16];
  makeUnits(units, 16);
  for (int i = 0; i < 16; i++) setHealthy(units[i]);
  char buf[16];
  size_t n = clusterFaultMaskHex(units, 16, buf, sizeof(buf));
  TEST_ASSERT_EQUAL(4, (int)n);
  TEST_ASSERT_EQUAL_STRING("0000", buf);
}

static void test_fault_mask_bit_is_unit_index() {
  UnitFacts units[16];
  makeUnits(units, 16);
  for (int i = 0; i < 16; i++) setHealthy(units[i]);
  setFaulty(units[2]);
  char buf[16];
  clusterFaultMaskHex(units, 16, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("0004", buf);
}

static void test_fault_mask_width_rounds_nibbles_up() {
  UnitFacts units[5];
  makeUnits(units, 5);
  for (int i = 0; i < 5; i++) setHealthy(units[i]);
  setFaulty(units[4]);
  char buf[16];
  size_t n = clusterFaultMaskHex(units, 5, buf, sizeof(buf));
  TEST_ASSERT_EQUAL(2, (int)n);
  TEST_ASSERT_EQUAL_STRING("10", buf);
}

static void test_fault_mask_unread_unit_is_not_faulty() {
  // statusValid false (silent / bootloader / old fw) must not set a bit —
  // same rule as computeFaultyUnitCount.
  UnitFacts units[4];
  makeUnits(units, 4);
  units[1].status.flags = UNIT_FLAG_LAST_HOME_FAILED;  // but statusValid false
  char buf[16];
  clusterFaultMaskHex(units, 4, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("0", buf);
}

static void test_fault_mask_zero_width_is_empty() {
  UnitFacts units[1];
  makeUnits(units, 1);
  char buf[16];
  size_t n = clusterFaultMaskHex(units, 0, buf, sizeof(buf));
  TEST_ASSERT_EQUAL(0, (int)n);
  TEST_ASSERT_EQUAL_STRING("", buf);
}

static void test_fault_mask_tiny_buffer_truncates_safely() {
  UnitFacts units[16];
  makeUnits(units, 16);
  char buf[3];  // needs 4 nibbles + NUL
  size_t n = clusterFaultMaskHex(units, 16, buf, sizeof(buf));
  TEST_ASSERT_TRUE(n < sizeof(buf));
  TEST_ASSERT_EQUAL('\0', buf[n]);
}

// --- ping-reply health fragment ------------------------------------------------

static void test_ping_health_fragment_shape() {
  UnitFacts units[16];
  makeUnits(units, 16);
  for (int i = 0; i < 16; i++) setHealthy(units[i]);
  setFaulty(units[0]);
  String frag = clusterPingHealthJson(units, 16, 16, 1, false, "abc1234");
  TEST_ASSERT_EQUAL_STRING(
      ",\"width\":16,\"detected\":16,\"faulty\":1,\"faultMask\":\"0001\","
      "\"wear\":false,\"rev\":\"abc1234\"",
      frag.c_str());
}

static void test_ping_health_fragment_wear_true() {
  UnitFacts units[1];
  makeUnits(units, 1);
  String frag = clusterPingHealthJson(units, 1, 1, 0, true, "abc1234");
  TEST_ASSERT_TRUE(frag.indexOf("\"wear\":true") >= 0);
}

// --- ping-reply health parse (leader side) --------------------------------------

static void test_parse_ping_health_full_body() {
  ClusterMemberHealth h;
  bool ok = clusterParsePingHealth(
      "{\"state\":\"clustered\",\"epoch\":1,\"seq\":2,\"width\":16,"
      "\"detected\":15,\"faulty\":2,\"faultMask\":\"0104\",\"wear\":true,"
      "\"rev\":\"abc1234\"}",
      h);
  TEST_ASSERT_TRUE(ok);
  TEST_ASSERT_TRUE(h.valid);
  TEST_ASSERT_EQUAL(2, h.faulty);
  TEST_ASSERT_EQUAL(15, h.detected);
  TEST_ASSERT_EQUAL_STRING("0104", h.faultMask.c_str());
  TEST_ASSERT_TRUE(h.wear);
}

static void test_parse_ping_health_old_firmware_reply_is_invalid() {
  // Pre-#294 followers answer only state/epoch/seq — the leader must fold
  // that as "no health", never as zero faults.
  ClusterMemberHealth h;
  bool ok = clusterParsePingHealth(
      "{\"state\":\"clustered\",\"epoch\":1,\"seq\":2}", h);
  TEST_ASSERT_FALSE(ok);
  TEST_ASSERT_FALSE(h.valid);
}

static void test_extract_json_bool() {
  String body = "{\"a\":true,\"b\":false}";
  TEST_ASSERT_TRUE(clusterExtractJsonBool(body, "a", false));
  TEST_ASSERT_FALSE(clusterExtractJsonBool(body, "b", true));
  TEST_ASSERT_TRUE(clusterExtractJsonBool(body, "missing", true));
  TEST_ASSERT_FALSE(clusterExtractJsonBool(body, "missing", false));
}

// --- shared /cluster/status serializer -------------------------------------------

static ClusterLeaderStatus makeStatus() {
  ClusterLeaderStatus st;
  st.enabled = true;
  st.epoch = 7;
  st.seq = 3;
  st.memberCount = 2;
  st.gridRows = 2;
  st.gridCapacity = 32;
  st.members[0].host = "";
  st.members[0].row = 0;
  st.members[0].width = 16;
  st.members[0].joined = true;
  st.members[0].healthValid = true;
  st.members[0].faulty = 0;
  st.members[0].detected = 16;
  st.members[0].faultMask = "0000";
  st.members[0].wear = false;
  st.members[1].host = "192.168.15.91";
  st.members[1].row = 1;
  st.members[1].width = 16;
  st.members[1].joined = true;
  st.members[1].rev = "abc1234";
  st.members[1].reportedWidth = 16;
  st.members[1].hmac = true;  // #313 follow-on: leader signs to this member
  st.rolloutPhase = "idle";
  return st;
}

static void test_status_json_carries_member_health() {
  String out = clusterStatusJson(makeStatus());
  TEST_ASSERT_TRUE(out.indexOf("\"enabled\":true") >= 0);
  TEST_ASSERT_TRUE(out.indexOf("\"faultMask\":\"0000\"") >= 0);
  TEST_ASSERT_TRUE(out.indexOf("\"faulty\":0") >= 0);
  TEST_ASSERT_TRUE(out.indexOf("\"detected\":16") >= 0);
  TEST_ASSERT_TRUE(out.indexOf("\"wear\":false") >= 0);
  TEST_ASSERT_TRUE(out.indexOf("\"rollout\":{\"phase\":\"idle\"") >= 0);
}

static void test_status_json_omits_health_when_invalid() {
  // Member 1 never delivered a #294 ping reply — no health keys at all for
  // it (the UI hides the strip on absence, it must not read zeros).
  String out = clusterStatusJson(makeStatus());
  int m1 = out.indexOf("192.168.15.91");
  TEST_ASSERT_TRUE(m1 >= 0);
  String tail = out.substring(m1);
  TEST_ASSERT_TRUE(tail.indexOf("\"faultMask\"") < 0);
  TEST_ASSERT_TRUE(tail.indexOf("\"faulty\"") < 0);
}

static void test_status_json_keeps_the_277_wire_keys() {
  // The Cluster card's pill renderer keys off these — the #294 additions
  // must stay additive.
  String out = clusterStatusJson(makeStatus());
  TEST_ASSERT_TRUE(out.indexOf("\"self\":true") >= 0);
  TEST_ASSERT_TRUE(out.indexOf("\"joined\":true") >= 0);
  TEST_ASSERT_TRUE(out.indexOf("\"updating\":false") >= 0);
  TEST_ASSERT_TRUE(out.indexOf("\"updateBlocked\":false") >= 0);
  TEST_ASSERT_TRUE(out.indexOf("\"hmac\":true") >= 0);
  TEST_ASSERT_TRUE(out.indexOf("\"imageVerifyFailed\":false") >= 0);
}

static void test_status_json_carries_plat_only_when_reported() {
  // #297: plat is additive — the S3 fleet never reports one (absent = same
  // platform as the leader); an ESP-01 member's reported plat surfaces so
  // the Cluster card can tag the row.
  ClusterLeaderStatus st = makeStatus();
  String out = clusterStatusJson(st);
  TEST_ASSERT_TRUE(out.indexOf("\"plat\"") < 0);
  st.members[1].plat = "esp01";
  out = clusterStatusJson(st);
  TEST_ASSERT_TRUE(out.indexOf("\"plat\":\"esp01\"") >= 0);
}

// --- digest ----------------------------------------------------------------------

static void test_digest_wraps_status_with_wall_context() {
  ClusterLeaderStatus st = makeStatus();
  String rows[2] = {"HELLO WALL      ", "SECOND ROW      "};
  String out = clusterBuildDigest(9, "bench-leader", "192.168.15.90",
                                  "|0|0|16;192.168.15.91|1|0|16", rows, 2, st);
  TEST_ASSERT_TRUE(out.startsWith("{\"gen\":9,\"leader\":{\"name\":\"bench-leader\","
                                  "\"host\":\"192.168.15.90\"}"));
  TEST_ASSERT_TRUE(out.indexOf("\"table\":\"|0|0|16;192.168.15.91|1|0|16\"") >= 0);
  TEST_ASSERT_TRUE(out.indexOf("\"rows\":[\"HELLO WALL      \",\"SECOND ROW      \"]") >= 0);
  TEST_ASSERT_TRUE(out.indexOf("\"status\":{\"enabled\":true") >= 0);
}

static void test_digest_escapes_leader_name() {
  ClusterLeaderStatus st = makeStatus();
  String rows[1] = {"X"};
  String out = clusterBuildDigest(1, "we\"ird", "h", "t", rows, 1, st);
  TEST_ASSERT_TRUE(out.indexOf("\"name\":\"we\\\"ird\"") >= 0);
}

// --- promote transform (#295) ------------------------------------------------------

static void test_promote_swaps_self_and_dead_leader() {
  String out;
  bool ok = clusterPromoteTransform("|0|0|16;192.168.15.91|1|0|16", 1,
                                    "192.168.15.90", out);
  TEST_ASSERT_TRUE(ok);
  TEST_ASSERT_EQUAL_STRING("192.168.15.90|0|0|16;|1|0|16", out.c_str());
}

static void test_promote_orchestrator_leader_has_no_row_to_fill() {
  // The dead leader had no display row (pure orchestrator) — promote just
  // claims the self entry.
  String out;
  bool ok = clusterPromoteTransform(
      "192.168.15.91|0|0|16;192.168.15.92|1|0|16", 0, "10.0.0.9", out);
  TEST_ASSERT_TRUE(ok);
  TEST_ASSERT_EQUAL_STRING("|0|0|16;192.168.15.92|1|0|16", out.c_str());
}

static void test_promote_rejects_bad_self_index() {
  String out;
  TEST_ASSERT_FALSE(
      clusterPromoteTransform("|0|0|16;h|1|0|16", 2, "x", out));
  TEST_ASSERT_FALSE(
      clusterPromoteTransform("|0|0|16;h|1|0|16", -1, "x", out));
}

static void test_promote_rejects_self_index_that_is_already_the_leader() {
  String out;
  TEST_ASSERT_FALSE(
      clusterPromoteTransform("|0|0|16;h|1|0|16", 0, "x", out));
}

static void test_promote_rejects_when_leader_row_needs_a_host_it_lacks() {
  // An empty-host entry exists but we don't know the dead leader's host —
  // the transform would mint a second self row.
  String out;
  TEST_ASSERT_FALSE(clusterPromoteTransform("|0|0|16;h|1|0|16", 1, "", out));
}

static void test_promote_rejects_malformed_table() {
  String out;
  TEST_ASSERT_FALSE(clusterPromoteTransform("not-a-table", 0, "x", out));
}

// --- digest shape gate (review: the follower re-serves the digest raw) -------------

static void test_digest_shape_accepts_a_real_digest() {
  ClusterLeaderStatus st = makeStatus();
  String rows[2] = {"A", "B"};
  String digest = clusterBuildDigest(1, "L", "10.0.0.9", "|0|0|16", rows, 2, st);
  TEST_ASSERT_TRUE(clusterDigestShapeOk(digest));
}

static void test_digest_shape_rejects_trailing_top_level_data() {
  // `{},"digest":{...}` would inject fields into the /cluster/digest
  // wrapper the follower splices the raw string into.
  TEST_ASSERT_FALSE(clusterDigestShapeOk("{},\"digest\":{\"gen\":1}"));
}

static void test_digest_shape_rejects_unbalanced_and_garbage() {
  TEST_ASSERT_FALSE(clusterDigestShapeOk(""));
  TEST_ASSERT_FALSE(clusterDigestShapeOk("not json"));
  TEST_ASSERT_FALSE(clusterDigestShapeOk("{\"gen\":1"));
  TEST_ASSERT_FALSE(clusterDigestShapeOk("{\"gen\":1}}"));
  TEST_ASSERT_FALSE(clusterDigestShapeOk("[1,2]"));  // array, not object
}

static void test_digest_shape_handles_braces_inside_strings() {
  TEST_ASSERT_TRUE(clusterDigestShapeOk("{\"rows\":[\"}{\",\"\\\"{\"]}"));
}

static void test_digest_shape_caps_length() {
  String big = "{\"pad\":\"";
  while (big.length() <= CLUSTER_DIGEST_MAX_LEN) big += "xxxxxxxx";
  big += "\"}";
  TEST_ASSERT_FALSE(clusterDigestShapeOk(big));
}

// --- old-leader demote marker (#295) -----------------------------------------------

static void test_join_rejected_other_leader_marker() {
  TEST_ASSERT_TRUE(clusterJoinRejectedOtherLeader(
      "{\"error\":\"other-leader\",\"leaderHost\":\"10.0.0.9\","
      "\"leaderName\":\"x\"}"));
  TEST_ASSERT_FALSE(
      clusterJoinRejectedOtherLeader("{\"error\":\"not clustered\"}"));
  TEST_ASSERT_FALSE(clusterJoinRejectedOtherLeader(""));
}

// --- CORS origin validator (#294 rung 3) -------------------------------------------

static void test_cors_allows_private_lan_origins() {
  TEST_ASSERT_TRUE(clusterCorsOriginAllowed("http://192.168.15.90"));
  TEST_ASSERT_TRUE(clusterCorsOriginAllowed("http://192.168.15.90:8080"));
  TEST_ASSERT_TRUE(clusterCorsOriginAllowed("http://10.0.0.5"));
  TEST_ASSERT_TRUE(clusterCorsOriginAllowed("http://172.16.0.1"));
  TEST_ASSERT_TRUE(clusterCorsOriginAllowed("http://172.31.255.9"));
  TEST_ASSERT_TRUE(clusterCorsOriginAllowed("http://127.0.0.1:5173"));
  TEST_ASSERT_TRUE(clusterCorsOriginAllowed("http://localhost"));
  TEST_ASSERT_TRUE(clusterCorsOriginAllowed("http://split-flap-a47dee.local"));
  TEST_ASSERT_TRUE(clusterCorsOriginAllowed("http://Split-Flap.LOCAL"));
}

static void test_cors_rejects_public_and_garbage_origins() {
  TEST_ASSERT_FALSE(clusterCorsOriginAllowed(""));
  TEST_ASSERT_FALSE(clusterCorsOriginAllowed("null"));
  TEST_ASSERT_FALSE(clusterCorsOriginAllowed("http://evil.com"));
  TEST_ASSERT_FALSE(clusterCorsOriginAllowed("http://8.8.8.8"));
  TEST_ASSERT_FALSE(clusterCorsOriginAllowed("http://172.32.0.1"));
  TEST_ASSERT_FALSE(clusterCorsOriginAllowed("http://192.168.1.evil.com"));
  TEST_ASSERT_FALSE(clusterCorsOriginAllowed("https://192.168.15.90"));
  TEST_ASSERT_FALSE(clusterCorsOriginAllowed("http://local"));
  TEST_ASSERT_FALSE(clusterCorsOriginAllowed("ftp://192.168.15.90"));
}

static void test_cors_path_surface_is_the_per_member_one() {
  // Rung 3's browser fan-out surface — and nothing else. Notably absent:
  // /firmware/* (fleet convergence owns cross-board updates), /cluster/*
  // (leader-driven wire), WiFi endpoints (off-limits from the wall UI).
  TEST_ASSERT_TRUE(clusterCorsPathAllowed("/settings"));
  // "/" is the per-card settings save (POST with ajax=1) — the panel's
  // device-name edit rides it. GETs of "/" just serve the page; harmless.
  TEST_ASSERT_TRUE(clusterCorsPathAllowed("/"));
  TEST_ASSERT_TRUE(clusterCorsPathAllowed("/units/health"));
  TEST_ASSERT_TRUE(clusterCorsPathAllowed("/units/health/refresh"));
  TEST_ASSERT_TRUE(clusterCorsPathAllowed("/unit/jog"));
  TEST_ASSERT_TRUE(clusterCorsPathAllowed("/unit/op-result"));
  TEST_ASSERT_TRUE(clusterCorsPathAllowed("/system/stats"));
  TEST_ASSERT_TRUE(clusterCorsPathAllowed("/log"));
  TEST_ASSERT_TRUE(clusterCorsPathAllowed("/log/flash"));
  TEST_ASSERT_TRUE(clusterCorsPathAllowed("/reboot"));
  // #304: board-level unit reflash from the wall panel (any member type).
  TEST_ASSERT_TRUE(clusterCorsPathAllowed("/reflash-units"));
  // /firmware/master stays closed here (unlike the ESP-01 follower's copy):
  // S3 members get their cross-board updates via #276 fleet convergence, so
  // the wall never browser-uploads to them.
  TEST_ASSERT_FALSE(clusterCorsPathAllowed("/firmware/master"));
  TEST_ASSERT_FALSE(clusterCorsPathAllowed("/cluster/config"));
  TEST_ASSERT_FALSE(clusterCorsPathAllowed("/cluster/promote"));
  TEST_ASSERT_FALSE(clusterCorsPathAllowed("/reset-wifi"));
  TEST_ASSERT_FALSE(clusterCorsPathAllowed("/log/flash/clear"));
  TEST_ASSERT_FALSE(clusterCorsPathAllowed("/unitx"));
}

static void test_cors_rejects_lookalike_ipv4() {
  TEST_ASSERT_FALSE(clusterCorsOriginAllowed("http://192.168.1"));
  TEST_ASSERT_FALSE(clusterCorsOriginAllowed("http://192.168.1.1.1"));
  TEST_ASSERT_FALSE(clusterCorsOriginAllowed("http://192.168.01x.1"));
  TEST_ASSERT_FALSE(clusterCorsOriginAllowed("http://1921.68.1.1"));
}

static void test_csrf_gate_blocks_cross_site_post() {
  // #313: a POST from a public/https page is forgery — refuse.
  TEST_ASSERT_TRUE(
      clusterCsrfRejectPost(true, true, "https://192.168.1.5"));  // not http
  TEST_ASSERT_TRUE(clusterCsrfRejectPost(true, true, "http://evil.example.com"));
  TEST_ASSERT_TRUE(clusterCsrfRejectPost(true, true, "http://8.8.8.8"));
}

static void test_csrf_gate_allows_lan_ui_and_server_to_server() {
  // The board's own LAN web UI carries a LAN origin — allowed.
  TEST_ASSERT_FALSE(clusterCsrfRejectPost(true, true, "http://192.168.1.5"));
  TEST_ASSERT_FALSE(clusterCsrfRejectPost(true, true, "http://splitflap.local"));
  // The leader's esp_http_client sends no Origin header — allowed.
  TEST_ASSERT_FALSE(clusterCsrfRejectPost(true, false, ""));
  // Never blocks a GET (safe method), even with a hostile origin.
  TEST_ASSERT_FALSE(clusterCsrfRejectPost(false, true, "http://evil.example.com"));
}

// #321 eligible-successor list -------------------------------------------------

static void test_successor_list_includes_s3_followers() {
  ClusterLeaderStatus st = makeStatus();  // member 0 = self, member 1 = S3 peer
  // Self is excluded; the S3 follower (empty plat = same platform) is index 1.
  TEST_ASSERT_EQUAL_STRING("1", clusterSuccessorList(st).c_str());
}

static void test_successor_list_excludes_esp01() {
  ClusterLeaderStatus st = makeStatus();
  st.members[1].plat = "esp01";  // foreign — never a takeover candidate
  TEST_ASSERT_EQUAL_STRING("", clusterSuccessorList(st).c_str());
}

static void test_successor_list_orders_multiple_s3_by_index() {
  ClusterLeaderStatus st = makeStatus();
  st.memberCount = 4;
  st.members[2].host = "192.168.1.50";
  st.members[2].plat = "esp32s3";
  st.members[2].width = 16;  // rendering row — width now carries meaning (#333)
  st.members[3].host = "192.168.1.51";
  st.members[3].plat = "esp01";  // excluded
  TEST_ASSERT_EQUAL_STRING("1,2", clusterSuccessorList(st).c_str());
}

// #333: a width-0 warm-standby backup outranks the rendering rows — it has
// no row of its own to lose, so it is the preferred successor (rank 0).
static void test_successor_list_prefers_zero_width_backup() {
  ClusterLeaderStatus st = makeStatus();
  st.memberCount = 3;
  st.members[2].host = "192.168.1.50";
  st.members[2].plat = "";   // same platform (S3)
  st.members[2].width = 0;   // warm-standby backup
  // Backup (index 2) leads the rendering follower (index 1).
  TEST_ASSERT_EQUAL_STRING("2,1", clusterSuccessorList(st).c_str());
}

// #332: role-aware succession tiers — backup > rendering display rows >
// spare > monitor. Role rides the join/ping replies (absent = pre-#332 peer
// or display); a monitor is a true last resort (promoting it kills the
// dashboard) but still saves an all-ESP-01 wall.
static void test_successor_role_tiers_full_order() {
  ClusterLeaderStatus st = makeStatus();
  st.memberCount = 5;
  st.members[1].role = "display";  // rendering row, width 16 (fixture)
  st.members[2].host = "192.168.1.50";
  st.members[2].plat = "";
  st.members[2].width = 0;
  st.members[2].role = "headless-monitor";
  st.members[3].host = "192.168.1.51";
  st.members[3].plat = "";
  st.members[3].width = 0;
  st.members[3].role = "headless-backup";
  st.members[4].host = "192.168.1.52";
  st.members[4].plat = "";
  st.members[4].width = 0;
  st.members[4].role = "headless-spare";
  // backup(3) > rendering(1) > spare(4) > monitor(2)
  TEST_ASSERT_EQUAL_STRING("3,1,4,2", clusterSuccessorList(st).c_str());
}

static void test_successor_unknown_role_width0_keeps_backup_tier() {
  // Pre-#332 peer: width 0, role absent — keeps today's preferred-successor
  // slot so a mixed-rev cluster behaves exactly as before convergence.
  ClusterLeaderStatus st = makeStatus();
  st.memberCount = 3;
  st.members[2].host = "192.168.1.50";
  st.members[2].plat = "";
  st.members[2].width = 0;
  st.members[2].role = "";
  TEST_ASSERT_EQUAL_STRING("2,1", clusterSuccessorList(st).c_str());
}

static void test_successor_width0_display_role_ranks_with_spare() {
  // A display-role board reporting width 0 (unit-less) must not vanish from
  // the list — it ranks with spare, below real rendering rows.
  ClusterLeaderStatus st = makeStatus();
  st.memberCount = 3;
  st.members[2].host = "192.168.1.50";
  st.members[2].plat = "";
  st.members[2].width = 0;
  st.members[2].role = "display";
  TEST_ASSERT_EQUAL_STRING("1,2", clusterSuccessorList(st).c_str());
}

static void test_successor_monitor_alone_still_listed() {
  ClusterLeaderStatus st = makeStatus();
  st.members[1].width = 0;
  st.members[1].role = "headless-monitor";
  TEST_ASSERT_EQUAL_STRING("1", clusterSuccessorList(st).c_str());
}

static void test_status_members_carry_role() {
  ClusterLeaderStatus st = makeStatus();
  st.members[1].role = "headless-monitor";
  String s = clusterStatusJson(st);
  TEST_ASSERT_TRUE(s.indexOf("\"role\":\"headless-monitor\"") >= 0);
  // The self row / roleless members emit no role key (additive, like plat).
  TEST_ASSERT_TRUE(s.indexOf("\"role\":\"\"") < 0);
}

static void test_digest_carries_succ_field() {
  ClusterLeaderStatus st = makeStatus();
  String rows[1] = {"HELLO"};
  String d = clusterBuildDigest(3, "wall", "192.168.1.2", "a|0|0|16", rows, 1, st);
  TEST_ASSERT_TRUE(d.indexOf("\"succ\":\"1\"") >= 0);
  TEST_ASSERT_TRUE(d.indexOf("\"hold\":0") >= 0);  // #321: 0 unless rebooting
}

// #337: the leader's active deviceMode rides the digest so a promoted
// successor resumes the cluster mode instead of its own NVS default.
static void test_digest_carries_mode_field() {
  ClusterLeaderStatus st = makeStatus();
  String rows[1] = {"HELLO"};
  String d = clusterBuildDigest(3, "wall", "192.168.1.2", "a|0|0|16", rows, 1, st,
                                0, "clock");
  TEST_ASSERT_TRUE(d.indexOf("\"mode\":\"clock\"") >= 0);
}

static void test_digest_mode_defaults_empty() {
  ClusterLeaderStatus st = makeStatus();
  String rows[1] = {"HELLO"};
  String d = clusterBuildDigest(3, "wall", "192.168.1.2", "a|0|0|16", rows, 1, st);
  TEST_ASSERT_TRUE(d.indexOf("\"mode\":\"\"") >= 0);
}

static void test_digest_carries_hold_when_rebooting() {
  ClusterLeaderStatus st = makeStatus();
  String rows[1] = {"HELLO"};
  String d = clusterBuildDigest(3, "wall", "192.168.1.2", "a|0|0|16", rows, 1, st,
                                45000);
  TEST_ASSERT_TRUE(d.indexOf("\"hold\":45000") >= 0);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_successor_list_includes_s3_followers);
  RUN_TEST(test_successor_list_excludes_esp01);
  RUN_TEST(test_successor_list_orders_multiple_s3_by_index);
  RUN_TEST(test_successor_list_prefers_zero_width_backup);
  RUN_TEST(test_successor_role_tiers_full_order);
  RUN_TEST(test_successor_unknown_role_width0_keeps_backup_tier);
  RUN_TEST(test_successor_width0_display_role_ranks_with_spare);
  RUN_TEST(test_successor_monitor_alone_still_listed);
  RUN_TEST(test_status_members_carry_role);
  RUN_TEST(test_digest_carries_succ_field);
  RUN_TEST(test_digest_carries_mode_field);
  RUN_TEST(test_digest_mode_defaults_empty);
  RUN_TEST(test_digest_carries_hold_when_rebooting);
  RUN_TEST(test_fault_mask_all_healthy_is_zeroes);
  RUN_TEST(test_fault_mask_bit_is_unit_index);
  RUN_TEST(test_fault_mask_width_rounds_nibbles_up);
  RUN_TEST(test_fault_mask_unread_unit_is_not_faulty);
  RUN_TEST(test_fault_mask_zero_width_is_empty);
  RUN_TEST(test_fault_mask_tiny_buffer_truncates_safely);
  RUN_TEST(test_ping_health_fragment_shape);
  RUN_TEST(test_ping_health_fragment_wear_true);
  RUN_TEST(test_parse_ping_health_full_body);
  RUN_TEST(test_parse_ping_health_old_firmware_reply_is_invalid);
  RUN_TEST(test_extract_json_bool);
  RUN_TEST(test_status_json_carries_member_health);
  RUN_TEST(test_status_json_omits_health_when_invalid);
  RUN_TEST(test_status_json_keeps_the_277_wire_keys);
  RUN_TEST(test_status_json_carries_plat_only_when_reported);
  RUN_TEST(test_digest_wraps_status_with_wall_context);
  RUN_TEST(test_digest_escapes_leader_name);
  RUN_TEST(test_promote_swaps_self_and_dead_leader);
  RUN_TEST(test_promote_orchestrator_leader_has_no_row_to_fill);
  RUN_TEST(test_promote_rejects_bad_self_index);
  RUN_TEST(test_promote_rejects_self_index_that_is_already_the_leader);
  RUN_TEST(test_promote_rejects_when_leader_row_needs_a_host_it_lacks);
  RUN_TEST(test_promote_rejects_malformed_table);
  RUN_TEST(test_digest_shape_accepts_a_real_digest);
  RUN_TEST(test_digest_shape_rejects_trailing_top_level_data);
  RUN_TEST(test_digest_shape_rejects_unbalanced_and_garbage);
  RUN_TEST(test_digest_shape_handles_braces_inside_strings);
  RUN_TEST(test_digest_shape_caps_length);
  RUN_TEST(test_join_rejected_other_leader_marker);
  RUN_TEST(test_cors_allows_private_lan_origins);
  RUN_TEST(test_cors_rejects_public_and_garbage_origins);
  RUN_TEST(test_cors_rejects_lookalike_ipv4);
  RUN_TEST(test_csrf_gate_blocks_cross_site_post);
  RUN_TEST(test_csrf_gate_allows_lan_ui_and_server_to_server);
  RUN_TEST(test_cors_path_surface_is_the_per_member_one);
  return UNITY_END();
}
