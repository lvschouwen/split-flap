// Host-side tests for the leader-side cluster supervision policy (#273) —
// member-table NVS wire format round-trips, per-member action scheduling
// (join → render → ping), failure backoff → degraded, and re-join
// recovery semantics.

#include <unity.h>

#include "../../ClusterLeaderPolicy.h"

void setUp() {}
void tearDown() {}

static ClusterMemberRuntime makeJoined(uint32_t nowMs) {
  ClusterMemberRuntime m;
  clusterMemberOnSuccess(m, nowMs);
  m.joined = true;
  return m;
}

// --- action scheduling ---------------------------------------------------------

static void test_fresh_member_needs_join() {
  ClusterMemberRuntime m;
  TEST_ASSERT_EQUAL(ClusterLeaderAction::Join,
                    clusterMemberNextAction(m, 1000));
}

static void test_joined_member_with_dirty_segment_renders() {
  ClusterMemberRuntime m = makeJoined(1000);
  m.renderDirty = true;
  TEST_ASSERT_EQUAL(ClusterLeaderAction::Render,
                    clusterMemberNextAction(m, 2000));
}

static void test_idle_member_pings_at_cadence() {
  ClusterMemberRuntime m = makeJoined(1000);
  TEST_ASSERT_EQUAL(ClusterLeaderAction::None,
                    clusterMemberNextAction(m, 1000 + CLUSTER_LEADER_PING_MS - 1));
  TEST_ASSERT_EQUAL(ClusterLeaderAction::Ping,
                    clusterMemberNextAction(m, 1000 + CLUSTER_LEADER_PING_MS));
}

static void test_render_outranks_ping() {
  ClusterMemberRuntime m = makeJoined(1000);
  m.renderDirty = true;
  TEST_ASSERT_EQUAL(ClusterLeaderAction::Render,
                    clusterMemberNextAction(m, 1000 + CLUSTER_LEADER_PING_MS));
}

static void test_backoff_gate_outranks_everything() {
  ClusterMemberRuntime m;
  clusterMemberOnFailure(m, 1000, true);  // nextAttempt pushed out
  TEST_ASSERT_EQUAL(ClusterLeaderAction::None,
                    clusterMemberNextAction(m, 1500));
  TEST_ASSERT_EQUAL(ClusterLeaderAction::Join,
                    clusterMemberNextAction(m, 1000 + CLUSTER_RETRY_BASE_MS));
}

// --- reply parsing (#297 platform key) -------------------------------------------

static void test_join_reply_plat_parses_and_defaults_empty() {
  // #297: absent plat = same platform as the leader (the whole pre-#297 S3
  // fleet); an ESP-01 row reports "esp01" and the rollout must skip it.
  ClusterMemberRuntime m;
  TEST_ASSERT_EQUAL_UINT(0, m.plat.length());
  String esp01 =
      "{\"name\":\"row-2\",\"rev\":\"abc1234\",\"width\":8,\"plat\":\"esp01\"}";
  m.plat = clusterExtractJsonString(esp01, "plat");
  TEST_ASSERT_EQUAL_STRING("esp01", m.plat.c_str());
  String s3 = "{\"name\":\"row-2\",\"rev\":\"abc1234\",\"width\":16}";
  TEST_ASSERT_EQUAL_STRING("", clusterExtractJsonString(s3, "plat").c_str());
}

// --- failure / recovery (#385 contact-age degrade) --------------------------------

static void test_backoff_doubles_and_caps() {
  ClusterMemberRuntime m;
  clusterMemberOnFailure(m, 1000, true);
  TEST_ASSERT_EQUAL_UINT32(1000 + 1000, m.nextAttemptMs);
  clusterMemberOnFailure(m, 3000, true);
  TEST_ASSERT_EQUAL_UINT32(3000 + 2000, m.nextAttemptMs);
  clusterMemberOnFailure(m, 6000, true);
  TEST_ASSERT_EQUAL_UINT32(6000 + 4000, m.nextAttemptMs);
  clusterMemberOnFailure(m, 11000, true);
  TEST_ASSERT_EQUAL_UINT32(11000 + 8000, m.nextAttemptMs);
  clusterMemberOnFailure(m, 20000, true);  // capped
  TEST_ASSERT_EQUAL_UINT32(20000 + CLUSTER_RETRY_MAX_MS, m.nextAttemptMs);
}

// #385: transient follower stalls (render apply, heartbeat collision, drift
// re-home — any class) produce scattered timeouts but never 30 s of silence.
// The member goes suspect (quiet: membership kept, retries continue) and must
// NOT degrade, no matter how many failures accumulate inside the window.
static void test_stall_failures_stay_suspect_never_degrade() {
  ClusterMemberRuntime m = makeJoined(1000);
  clusterMemberOnFailure(m, 2000, true);
  clusterMemberOnFailure(m, 2600, true);
  clusterMemberOnFailure(m, 3400, true);
  clusterMemberOnFailure(m, 15000, true);
  TEST_ASSERT_TRUE(m.failures > 0);
  TEST_ASSERT_TRUE(clusterMemberSuspect(m));
  TEST_ASSERT_FALSE(m.degraded);
  TEST_ASSERT_TRUE(m.joined);  // membership + HMAC key survive suspect
  clusterMemberOnSuccess(m, 16000);
  TEST_ASSERT_FALSE(clusterMemberSuspect(m));
  TEST_ASSERT_EQUAL(0, m.failures);
}

// Degrade is wall-clock truth: a counted failure landing >= 30 s after the
// last successful round-trip (of ANY kind) degrades and forces the re-join.
static void test_thirty_seconds_of_silence_degrades() {
  ClusterMemberRuntime m = makeJoined(1000);
  clusterMemberOnFailure(m, 12000, true);
  clusterMemberOnFailure(m, 22000, true);
  clusterMemberOnFailure(m, 30999, true);  // age 29 999 ms — one short
  TEST_ASSERT_FALSE(m.degraded);
  clusterMemberOnFailure(m, 31000, true);  // age 30 000 ms
  TEST_ASSERT_TRUE(m.degraded);
  TEST_ASSERT_FALSE(m.joined);  // recovery is a fresh join
  TEST_ASSERT_FALSE(clusterMemberSuspect(m));  // degraded outranks suspect
  TEST_ASSERT_EQUAL(ClusterLeaderAction::Join,
                    clusterMemberNextAction(m, 31000 + CLUSTER_RETRY_MAX_MS));
}

static void test_success_clears_failures_and_degraded() {
  ClusterMemberRuntime m = makeJoined(1000);
  clusterMemberOnFailure(m, 32000, true);  // 31 s of silence: one strike is enough
  TEST_ASSERT_TRUE(m.degraded);
  clusterMemberOnSuccess(m, 40000);  // the fresh join's round-trip
  TEST_ASSERT_FALSE(m.degraded);
  TEST_ASSERT_EQUAL(0, m.failures);
  TEST_ASSERT_EQUAL_UINT32(40000, m.lastContactMs);
}

// #385 leader-offline gate: failures while the leader's own netif is down are
// the leader's problem, not member evidence — reschedule only, count nothing.
static void test_offline_failures_do_not_count() {
  ClusterMemberRuntime m = makeJoined(1000);
  clusterMemberOnFailure(m, 40000, false);
  clusterMemberOnFailure(m, 50000, false);
  TEST_ASSERT_EQUAL(0, m.failures);
  TEST_ASSERT_FALSE(m.degraded);
  TEST_ASSERT_FALSE(clusterMemberSuspect(m));
  TEST_ASSERT_EQUAL_UINT32(50000 + CLUSTER_RETRY_BASE_MS, m.nextAttemptMs);
}

// Epoch stamp = benefit of the doubt: member-table apply and netif recovery
// stamp lastContactMs so a never-contacted member (or the whole wall after a
// leader outage) gets the full 30 s window instead of degrading on its first
// post-epoch failure.
static void test_contact_epoch_stamp_resets_silence_window() {
  ClusterMemberRuntime m;  // fresh: lastContactMs == 0
  clusterMemberStampContactEpoch(m, 100000);
  clusterMemberOnFailure(m, 101000, true);
  TEST_ASSERT_FALSE(m.degraded);  // without the stamp the age would be ~100 s
  clusterMemberOnFailure(m, 100000 + CLUSTER_DEGRADED_SILENCE_MS, true);
  TEST_ASSERT_TRUE(m.degraded);  // a genuinely dead host degrades ~30 s after epoch
}

// #385 render-stuck: pings fine but renders failing never degrades under the
// age criterion (the member IS alive) — it surfaces as a distinct flag.
static void test_render_stuck_flags_persistent_dirty() {
  ClusterMemberRuntime m = makeJoined(1000);
  clusterMemberMarkRenderDirty(m, 5000);
  TEST_ASSERT_TRUE(m.renderDirty);
  TEST_ASSERT_FALSE(
      clusterMemberRenderStuck(m, 5000 + CLUSTER_DEGRADED_SILENCE_MS - 1));
  TEST_ASSERT_TRUE(
      clusterMemberRenderStuck(m, 5000 + CLUSTER_DEGRADED_SILENCE_MS));
  // Re-marking while already dirty keeps the ORIGINAL stamp — stuck is
  // measured from the first unmet want, not the latest content change.
  clusterMemberMarkRenderDirty(m, 20000);
  TEST_ASSERT_TRUE(
      clusterMemberRenderStuck(m, 5000 + CLUSTER_DEGRADED_SILENCE_MS));
  clusterMemberRenderAcked(m);  // ACK clears dirty + stamp
  TEST_ASSERT_FALSE(m.renderDirty);
  TEST_ASSERT_FALSE(clusterMemberRenderStuck(m, 60000));
}

static void test_not_clustered_reply_forces_rejoin_without_backoff() {
  ClusterMemberRuntime m = makeJoined(1000);
  clusterMemberOnNotClustered(m);
  TEST_ASSERT_FALSE(m.joined);
  TEST_ASSERT_FALSE(m.degraded);  // the follower answered — link is fine
  TEST_ASSERT_EQUAL(ClusterLeaderAction::Join,
                    clusterMemberNextAction(m, 1001));
}

// --- member-table wire format -----------------------------------------------------

static void test_table_round_trips_through_string() {
  ClusterMemberTable t;
  t.count = 3;
  // Leader's own row: empty host.
  t.members[0].host[0] = '\0';
  t.members[0].row = 0;
  t.members[0].col = 0;
  t.members[0].width = 16;
  snprintf(t.members[1].host, sizeof(t.members[1].host), "192.168.15.91");
  t.members[1].row = 1;
  t.members[1].col = 0;
  t.members[1].width = 16;
  snprintf(t.members[2].host, sizeof(t.members[2].host), "wall-3.local");
  t.members[2].row = 1;
  t.members[2].col = 16;
  t.members[2].width = 16;

  String stored = clusterTableToString(t);
  TEST_ASSERT_EQUAL_STRING("|0|0|16;192.168.15.91|1|0|16;wall-3.local|1|16|16",
                           stored.c_str());

  ClusterMemberTable parsed;
  TEST_ASSERT_TRUE(clusterTableFromString(stored, parsed));
  TEST_ASSERT_EQUAL(3, parsed.count);
  TEST_ASSERT_EQUAL_STRING("", parsed.members[0].host);
  TEST_ASSERT_EQUAL_STRING("192.168.15.91", parsed.members[1].host);
  TEST_ASSERT_EQUAL(1, parsed.members[1].row);
  TEST_ASSERT_EQUAL_STRING("wall-3.local", parsed.members[2].host);
  TEST_ASSERT_EQUAL(16, parsed.members[2].col);
  TEST_ASSERT_EQUAL(16, parsed.members[2].width);
}

static void test_empty_string_parses_to_disabled_table() {
  ClusterMemberTable t;
  TEST_ASSERT_TRUE(clusterTableFromString("", t));
  TEST_ASSERT_EQUAL(0, t.count);
}

static void test_malformed_entries_rejected() {
  ClusterMemberTable t;
  TEST_ASSERT_FALSE(clusterTableFromString("a|1|0", t));           // 3 fields
  TEST_ASSERT_FALSE(clusterTableFromString("a|1|0|16|9", t));      // 5 fields
  TEST_ASSERT_FALSE(clusterTableFromString("a|x|0|16", t));        // non-numeric
  TEST_ASSERT_FALSE(clusterTableFromString("a|1|0|300", t));       // width > 255
  TEST_ASSERT_FALSE(clusterTableFromString("a|-1|0|16", t));       // negative
  TEST_ASSERT_FALSE(clusterTableFromString("a b|1|0|16", t));      // space in host
  TEST_ASSERT_FALSE(clusterTableFromString("a\tb|1|0|16", t));     // control char
}

static void test_too_many_entries_rejected() {
  String stored;
  for (int i = 0; i < CLUSTER_MAX_MEMBERS + 1; i++) {
    if (i) stored += ';';
    stored += "h";
    stored += i;
    stored += "|0|0|16";
  }
  ClusterMemberTable t;
  TEST_ASSERT_FALSE(clusterTableFromString(stored, t));
}

static void test_oversized_host_rejected() {
  String host;
  for (int i = 0; i < CLUSTER_HOST_MAX_LEN + 1; i++) host += 'a';
  ClusterMemberTable t;
  TEST_ASSERT_FALSE(clusterTableFromString(host + "|0|0|16", t));
}

static void test_public_host_rejected_ssrf() {
  // #313: a member host is dialed with the leader's firmware — restrict to
  // LAN targets so a public host can't be smuggled in for exfiltration.
  ClusterMemberTable t;
  TEST_ASSERT_FALSE(clusterTableFromString("8.8.8.8|1|0|16", t));      // public IP
  TEST_ASSERT_FALSE(clusterTableFromString("evil.com|1|0|16", t));     // public name
  TEST_ASSERT_FALSE(clusterTableFromString("169.254.1.1|1|0|16", t));  // link-local
  TEST_ASSERT_FALSE(clusterTableFromString("172.32.0.1|1|0|16", t));   // just outside 172.16/12
  // LAN targets and the empty self-host all pass.
  TEST_ASSERT_TRUE(clusterTableFromString("|0|0|16", t));
  TEST_ASSERT_TRUE(clusterTableFromString("10.0.0.4|1|0|16", t));
  TEST_ASSERT_TRUE(clusterTableFromString("192.168.15.91|1|0|16", t));
  TEST_ASSERT_TRUE(clusterTableFromString("172.16.5.5|1|0|16", t));
  TEST_ASSERT_TRUE(clusterTableFromString("wall-3.local|1|0|16", t));
  TEST_ASSERT_TRUE(clusterTableFromString("localhost|1|0|16", t));
}

static void test_lan_target_classifier() {
  TEST_ASSERT_TRUE(clusterHostIsLanTarget("192.168.1.1"));
  TEST_ASSERT_TRUE(clusterHostIsLanTarget("10.255.0.1"));
  TEST_ASSERT_TRUE(clusterHostIsLanTarget("172.16.0.1"));
  TEST_ASSERT_TRUE(clusterHostIsLanTarget("172.31.255.255"));
  TEST_ASSERT_TRUE(clusterHostIsLanTarget("127.0.0.1"));
  TEST_ASSERT_TRUE(clusterHostIsLanTarget("Wall-3.LOCAL"));  // case-insensitive
  TEST_ASSERT_TRUE(clusterHostIsLanTarget("localhost"));
  TEST_ASSERT_FALSE(clusterHostIsLanTarget("172.15.0.1"));
  TEST_ASSERT_FALSE(clusterHostIsLanTarget("172.32.0.1"));
  TEST_ASSERT_FALSE(clusterHostIsLanTarget("8.8.8.8"));
  TEST_ASSERT_FALSE(clusterHostIsLanTarget(".local"));  // needs a label
  TEST_ASSERT_FALSE(clusterHostIsLanTarget("attacker.com"));
}

static void test_self_member_is_empty_host() {
  ClusterMemberDef self;
  self.host[0] = '\0';
  ClusterMemberDef remote;
  snprintf(remote.host, sizeof(remote.host), "wall-2.local");
  TEST_ASSERT_TRUE(clusterMemberIsSelf(self));
  TEST_ASSERT_FALSE(clusterMemberIsSelf(remote));
}


// --- join/ping reply parsing --------------------------------------------------------

static void test_json_field_extraction() {
  String body =
      "{\"name\":\"wall-2\",\"rev\":\"b0e3fe6\",\"width\":16,\"protocol\":1}";
  TEST_ASSERT_EQUAL_STRING("b0e3fe6",
                           clusterExtractJsonString(body, "rev").c_str());
  TEST_ASSERT_EQUAL_STRING("wall-2",
                           clusterExtractJsonString(body, "name").c_str());
  TEST_ASSERT_EQUAL(16, clusterExtractJsonInt(body, "width", -1));
  TEST_ASSERT_EQUAL_STRING("",
                           clusterExtractJsonString(body, "missing").c_str());
  TEST_ASSERT_EQUAL(-1, clusterExtractJsonInt(body, "missing", -1));
  // A key whose name suffixes another must not match it.
  TEST_ASSERT_EQUAL(-1, clusterExtractJsonInt(body, "idth", -1));
}

// --- one-op-per-tick fan-out round-robin (#320) --------------------------------

static void test_fanout_none_due_returns_negative() {
  int cursor = -1;
  bool none[3] = {false, false, false};
  TEST_ASSERT_EQUAL(-1, clusterFanoutNext(cursor, none, 3));
  TEST_ASSERT_EQUAL(-1, clusterFanoutNext(cursor, nullptr, 0));
}

static void test_fanout_serves_one_then_round_robins() {
  int cursor = -1;
  bool all[3] = {true, true, true};
  // One member per call, in order, wrapping — never all at once.
  TEST_ASSERT_EQUAL(0, clusterFanoutNext(cursor, all, 3));
  TEST_ASSERT_EQUAL(1, clusterFanoutNext(cursor, all, 3));
  TEST_ASSERT_EQUAL(2, clusterFanoutNext(cursor, all, 3));
  TEST_ASSERT_EQUAL(0, clusterFanoutNext(cursor, all, 3));  // wrap
}

static void test_fanout_skips_members_with_no_due_op() {
  int cursor = -1;
  bool some[3] = {true, false, true};
  TEST_ASSERT_EQUAL(0, clusterFanoutNext(cursor, some, 3));
  TEST_ASSERT_EQUAL(2, clusterFanoutNext(cursor, some, 3));  // 1 skipped
  TEST_ASSERT_EQUAL(0, clusterFanoutNext(cursor, some, 3));  // wrap past 1
}

// The regression that caused #320: a stuck member (a dead host that stays
// un-joined every tick) must not monopolize the fan-out — the healthy member
// still gets served every other tick.
static void test_fanout_stuck_member_does_not_starve_others() {
  int cursor = -1;
  bool stuck[2] = {true, true};  // index 0 = dead host, always due
  TEST_ASSERT_EQUAL(0, clusterFanoutNext(cursor, stuck, 2));
  TEST_ASSERT_EQUAL(1, clusterFanoutNext(cursor, stuck, 2));  // healthy served
  TEST_ASSERT_EQUAL(0, clusterFanoutNext(cursor, stuck, 2));
  TEST_ASSERT_EQUAL(1, clusterFanoutNext(cursor, stuck, 2));  // and again
}

// A membership shrink can leave the cursor past the new end; it must normalize
// rather than index out of bounds or skip everyone.
static void test_fanout_normalizes_stale_cursor() {
  int cursor = 5;  // stale: count is now 3
  bool some[3] = {false, true, false};
  TEST_ASSERT_EQUAL(1, clusterFanoutNext(cursor, some, 3));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_fresh_member_needs_join);
  RUN_TEST(test_joined_member_with_dirty_segment_renders);
  RUN_TEST(test_idle_member_pings_at_cadence);
  RUN_TEST(test_render_outranks_ping);
  RUN_TEST(test_backoff_gate_outranks_everything);
  RUN_TEST(test_backoff_doubles_and_caps);
  RUN_TEST(test_stall_failures_stay_suspect_never_degrade);
  RUN_TEST(test_thirty_seconds_of_silence_degrades);
  RUN_TEST(test_success_clears_failures_and_degraded);
  RUN_TEST(test_offline_failures_do_not_count);
  RUN_TEST(test_contact_epoch_stamp_resets_silence_window);
  RUN_TEST(test_render_stuck_flags_persistent_dirty);
  RUN_TEST(test_not_clustered_reply_forces_rejoin_without_backoff);
  RUN_TEST(test_table_round_trips_through_string);
  RUN_TEST(test_empty_string_parses_to_disabled_table);
  RUN_TEST(test_malformed_entries_rejected);
  RUN_TEST(test_too_many_entries_rejected);
  RUN_TEST(test_oversized_host_rejected);
  RUN_TEST(test_public_host_rejected_ssrf);
  RUN_TEST(test_lan_target_classifier);
  RUN_TEST(test_self_member_is_empty_host);
  RUN_TEST(test_json_field_extraction);
  RUN_TEST(test_join_reply_plat_parses_and_defaults_empty);
  RUN_TEST(test_fanout_none_due_returns_negative);
  RUN_TEST(test_fanout_serves_one_then_round_robins);
  RUN_TEST(test_fanout_skips_members_with_no_due_op);
  RUN_TEST(test_fanout_stuck_member_does_not_starve_others);
  RUN_TEST(test_fanout_normalizes_stale_cursor);
  return UNITY_END();
}
