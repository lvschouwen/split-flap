// Host-side tests for shared/ClusterWireGuards.h — the member side of the
// cluster wire as an S3 member and an ESP-01 row both run it: join
// validation, leader binding, leave authorisation, the wire-auth state and
// the health keys of a reply.

#include <ArduinoFake.h>
#include <unity.h>

#include "ClusterWireGuards.h"

void setUp() {}
void tearDown() {}

namespace {

const char* const LEADER = "192.168.15.88";

String keyHex(uint8_t fill) {
  uint8_t key[CLUSTER_HMAC_KEY_LEN];
  for (int i = 0; i < CLUSTER_HMAC_KEY_LEN; i++) key[i] = (uint8_t)(fill + i);
  return clusterKeyToHex(key);
}

String signedLeave(const String& hex, uint64_t ts) {
  uint8_t key[CLUSTER_HMAC_KEY_LEN];
  clusterKeyFromHex(hex, key);
  return clusterHmacSign(key, clusterHmacLeaveMsg(ts));
}

}  // namespace

// --- join -------------------------------------------------------------------

static void test_join_accepts_the_leader_dialling_from_its_own_address() {
  TEST_ASSERT_EQUAL((int)ClusterJoinCheck::Ok,
                    (int)clusterJoinValidate(0, LEADER, "wall", 32, LEADER));
  TEST_ASSERT_EQUAL(
      (int)ClusterJoinCheck::Ok,
      (int)clusterJoinValidate(CLUSTER_MAX_MEMBERS - 1, LEADER, "", 32, LEADER));
}

// One bound on every member: the leader's table has CLUSTER_MAX_MEMBERS rows.
static void test_join_row_is_bounded_by_the_member_table() {
  TEST_ASSERT_EQUAL(
      (int)ClusterJoinCheck::RowOutOfRange,
      (int)clusterJoinValidate(CLUSTER_MAX_MEMBERS, LEADER, "w", 32, LEADER));
  TEST_ASSERT_EQUAL((int)ClusterJoinCheck::RowOutOfRange,
                    (int)clusterJoinValidate(-1, LEADER, "w", 32, LEADER));
  TEST_ASSERT_EQUAL((int)ClusterJoinCheck::RowOutOfRange,
                    (int)clusterJoinValidate(255, LEADER, "w", 32, LEADER));
  TEST_ASSERT_EQUAL(400, clusterJoinCheckStatus(ClusterJoinCheck::RowOutOfRange));
}

static void test_join_refuses_bad_host_and_name() {
  TEST_ASSERT_EQUAL((int)ClusterJoinCheck::BadLeaderHost,
                    (int)clusterJoinValidate(0, "", "w", 32, ""));
  TEST_ASSERT_EQUAL(
      (int)ClusterJoinCheck::BadLeaderHost,
      (int)clusterJoinValidate(0, "host with space", "w", 32, "host with space"));
  String longHost;
  for (int i = 0; i <= CLUSTER_HOST_MAX_LEN; i++) longHost += 'a';
  TEST_ASSERT_EQUAL((int)ClusterJoinCheck::BadLeaderHost,
                    (int)clusterJoinValidate(0, longHost, "w", 32, longHost));
  TEST_ASSERT_EQUAL(
      (int)ClusterJoinCheck::BadLeaderName,
      (int)clusterJoinValidate(0, LEADER, "na\x01me", 32, LEADER));
  TEST_ASSERT_EQUAL(
      (int)ClusterJoinCheck::BadLeaderName,
      (int)clusterJoinValidate(0, LEADER, "a name longer than four", 4, LEADER));
  // A name may contain spaces; a host may not.
  TEST_ASSERT_EQUAL(
      (int)ClusterJoinCheck::Ok,
      (int)clusterJoinValidate(0, LEADER, "living room", 32, LEADER));
}

// #313: any other LAN host naming the leader cannot mint a membership.
static void test_join_from_another_address_is_403() {
  ClusterJoinCheck c =
      clusterJoinValidate(0, LEADER, "wall", 32, "192.168.15.77");
  TEST_ASSERT_EQUAL((int)ClusterJoinCheck::CallerIsNotLeaderHost, (int)c);
  TEST_ASSERT_EQUAL(403, clusterJoinCheckStatus(c));
}

// --- render / ping ----------------------------------------------------------

static void test_only_the_joined_leader_is_not_foreign() {
  TEST_ASSERT_FALSE(clusterCallerIsForeign(LEADER, LEADER));
  TEST_ASSERT_TRUE(clusterCallerIsForeign(LEADER, "192.168.15.77"));
  // No membership: nobody to compare against (the handler says "not
  // clustered").
  TEST_ASSERT_FALSE(clusterCallerIsForeign("", "192.168.15.77"));
}

// --- leave ------------------------------------------------------------------

static void test_unkeyed_leave_needs_the_leader_or_the_local_browser() {
  TEST_ASSERT_TRUE(clusterLeaveAllowed(false, false, LEADER, LEADER, false));
  TEST_ASSERT_TRUE(
      clusterLeaveAllowed(false, false, LEADER, "192.168.15.50", true));
  TEST_ASSERT_FALSE(
      clusterLeaveAllowed(false, false, LEADER, "192.168.15.50", false));
  // Not a member: leave is idempotent.
  TEST_ASSERT_TRUE(clusterLeaveAllowed(false, false, "", "192.168.15.50", false));
}

// A keyed member does not trust the leader's address: a spoofed IP without
// the signature is refused.
static void test_keyed_leave_needs_the_signature_or_the_local_browser() {
  TEST_ASSERT_FALSE(clusterLeaveAllowed(true, false, LEADER, LEADER, false));
  TEST_ASSERT_TRUE(clusterLeaveAllowed(true, true, LEADER, "10.0.0.9", false));
  TEST_ASSERT_TRUE(clusterLeaveAllowed(true, false, LEADER, "10.0.0.9", true));
}

// --- wire-auth state --------------------------------------------------------

static void test_adopting_a_key_turns_enforcement_on() {
  ClusterMemberAuth auth;
  TEST_ASSERT_FALSE(auth.keyed);
  TEST_ASSERT_TRUE(auth.adoptKey(keyHex(1)));
  TEST_ASSERT_TRUE(auth.keyed);
  TEST_ASSERT_FALSE(auth.adoptKey(keyHex(1)));  // same key: nothing changed
}

static void test_no_or_bad_key_turns_enforcement_off() {
  ClusterMemberAuth auth;
  auth.adoptKey(keyHex(1));
  TEST_ASSERT_TRUE(auth.adoptKey(""));
  TEST_ASSERT_FALSE(auth.keyed);
  auth.adoptKey(keyHex(1));
  TEST_ASSERT_TRUE(auth.adoptKey("not-hex"));
  TEST_ASSERT_FALSE(auth.keyed);
}

static void test_accept_verifies_and_refuses_a_replay() {
  ClusterMemberAuth auth;
  String hex = keyHex(7);
  auth.adoptKey(hex);
  bool due = false;
  uint64_t ts = 1700000000000ULL;
  TEST_ASSERT_TRUE(auth.accept(clusterHmacLeaveMsg(ts), ts, signedLeave(hex, ts),
                               ts, true, due));
  // The same request again is a replay.
  TEST_ASSERT_FALSE(auth.accept(clusterHmacLeaveMsg(ts), ts,
                                signedLeave(hex, ts), ts, true, due));
  // A different key's signature is refused.
  uint64_t ts2 = ts + 1000;
  TEST_ASSERT_FALSE(auth.accept(clusterHmacLeaveMsg(ts2), ts2,
                                signedLeave(keyHex(9), ts2), ts2, true, due));
}

static void test_unkeyed_member_accepts_nothing_signed() {
  ClusterMemberAuth auth;
  bool due = true;
  uint64_t ts = 1700000000000ULL;
  TEST_ASSERT_FALSE(auth.accept(clusterHmacLeaveMsg(ts), ts,
                                signedLeave(keyHex(1), ts), ts, true, due));
  TEST_ASSERT_FALSE(due);
}

// A rebooted leader mints a new key and signs from a lower ts: the mark must
// reset with the key, or every request of the new leader reads as a replay.
static void test_new_key_resets_the_replay_mark() {
  ClusterMemberAuth auth;
  String first = keyHex(1);
  auth.adoptKey(first);
  bool due = false;
  uint64_t late = 1700000900000ULL;
  TEST_ASSERT_TRUE(auth.accept(clusterHmacLeaveMsg(late), late,
                               signedLeave(first, late), late, true, due));
  String second = keyHex(2);
  TEST_ASSERT_TRUE(auth.adoptKey(second));
  TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)auth.lastAcceptedTs);
  TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)auth.lastPersistedTs);
  uint64_t early = late - 500000;
  TEST_ASSERT_TRUE(auth.accept(clusterHmacLeaveMsg(early), early,
                               signedLeave(second, early), early, true, due));
}

// The mark is written coarsely: due on the first accept, not again until it
// has moved far enough, and markPersisted() is what settles it.
static void test_mark_is_due_once_until_persisted() {
  ClusterMemberAuth auth;
  String hex = keyHex(3);
  auth.adoptKey(hex);
  bool due = false;
  uint64_t ts = 1700000000000ULL;
  auth.accept(clusterHmacLeaveMsg(ts), ts, signedLeave(hex, ts), ts, true, due);
  TEST_ASSERT_TRUE(due);
  auth.markPersisted();
  uint64_t next = ts + 10000;
  auth.accept(clusterHmacLeaveMsg(next), next, signedLeave(hex, next), next,
              true, due);
  TEST_ASSERT_FALSE(due);
}

static void test_restored_and_drop() {
  ClusterMemberAuth auth;
  auth.restored(true, 1234);
  TEST_ASSERT_TRUE(auth.keyed);
  TEST_ASSERT_EQUAL_UINT32(1234, (uint32_t)auth.lastAcceptedTs);
  TEST_ASSERT_EQUAL_UINT32(1234, (uint32_t)auth.lastPersistedTs);
  auth.drop();
  TEST_ASSERT_FALSE(auth.keyed);
  TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)auth.lastAcceptedTs);
  // A stored mark without a valid key means nothing.
  auth.restored(false, 99);
  TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)auth.lastAcceptedTs);
}

// --- reply health keys ------------------------------------------------------

static void test_health_keys_shape() {
  ClusterRowHealth h;
  h.width = 16;
  h.detected = 16;
  h.faulty = 1;
  h.faultMask = "0001";
  h.lost = 1;
  String out;
  clusterAppendHealthKeys(out, h);
  TEST_ASSERT_EQUAL_STRING(
      ",\"width\":16,\"detected\":16,\"faulty\":1,\"faultMask\":\"0001\","
      "\"lost\":1,\"wear\":false",
      out.c_str());
}

static void test_health_keys_bus_dead_is_additive() {
  ClusterRowHealth h;
  h.busDead = true;
  h.wear = true;
  String out;
  clusterAppendHealthKeys(out, h);
  TEST_ASSERT_TRUE(out.indexOf("\"busDead\":1,\"wear\":true") >= 0);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_join_accepts_the_leader_dialling_from_its_own_address);
  RUN_TEST(test_join_row_is_bounded_by_the_member_table);
  RUN_TEST(test_join_refuses_bad_host_and_name);
  RUN_TEST(test_join_from_another_address_is_403);
  RUN_TEST(test_only_the_joined_leader_is_not_foreign);
  RUN_TEST(test_unkeyed_leave_needs_the_leader_or_the_local_browser);
  RUN_TEST(test_keyed_leave_needs_the_signature_or_the_local_browser);
  RUN_TEST(test_adopting_a_key_turns_enforcement_on);
  RUN_TEST(test_no_or_bad_key_turns_enforcement_off);
  RUN_TEST(test_accept_verifies_and_refuses_a_replay);
  RUN_TEST(test_unkeyed_member_accepts_nothing_signed);
  RUN_TEST(test_new_key_resets_the_replay_mark);
  RUN_TEST(test_mark_is_due_once_until_persisted);
  RUN_TEST(test_restored_and_drop);
  RUN_TEST(test_health_keys_shape);
  RUN_TEST(test_health_keys_bus_dead_is_additive);
  return UNITY_END();
}
