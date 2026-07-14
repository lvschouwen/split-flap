// Host-side tests for the follower's EEPROM membership record (#298) — the
// only on-ESP persisted setting besides WiFi credentials (the `clusteredBy`
// marker: leader name/host + row, so a reboot lands in Grace, never flashes
// stale standalone content).

#include <cstring>

#include <unity.h>

#include "../../FollowerSettings.h"

void setUp() {}
void tearDown() {}

static void test_membership_round_trips() {
  uint8_t blob[FOLLOWER_MEMBERSHIP_BLOB_LEN];
  TEST_ASSERT_TRUE(followerMembershipEncode("wall-leader", "192.168.15.22", 3,
                                            blob));
  char name[FOLLOWER_NAME_MAX + 1];
  char host[FOLLOWER_HOST_MAX + 1];
  uint8_t row = 0;
  TEST_ASSERT_TRUE(followerMembershipDecode(blob, name, host, row));
  TEST_ASSERT_EQUAL_STRING("wall-leader", name);
  TEST_ASSERT_EQUAL_STRING("192.168.15.22", host);
  TEST_ASSERT_EQUAL_UINT8(3, row);
}

static void test_blank_eeprom_decodes_to_no_membership() {
  uint8_t blob[FOLLOWER_MEMBERSHIP_BLOB_LEN];
  memset(blob, 0xFF, sizeof(blob));  // factory-fresh flash
  char name[FOLLOWER_NAME_MAX + 1];
  char host[FOLLOWER_HOST_MAX + 1];
  uint8_t row = 0;
  TEST_ASSERT_FALSE(followerMembershipDecode(blob, name, host, row));
  memset(blob, 0x00, sizeof(blob));
  TEST_ASSERT_FALSE(followerMembershipDecode(blob, name, host, row));
}

static void test_corrupt_checksum_rejects() {
  uint8_t blob[FOLLOWER_MEMBERSHIP_BLOB_LEN];
  followerMembershipEncode("leader", "10.0.0.9", 1, blob);
  blob[10] ^= 0x40;
  char name[FOLLOWER_NAME_MAX + 1];
  char host[FOLLOWER_HOST_MAX + 1];
  uint8_t row = 0;
  TEST_ASSERT_FALSE(followerMembershipDecode(blob, name, host, row));
}

static void test_oversized_fields_reject_at_encode() {
  uint8_t blob[FOLLOWER_MEMBERSHIP_BLOB_LEN];
  char longHost[FOLLOWER_HOST_MAX + 8];
  memset(longHost, 'a', sizeof(longHost) - 1);
  longHost[sizeof(longHost) - 1] = '\0';
  TEST_ASSERT_FALSE(followerMembershipEncode("n", longHost, 0, blob));
  char longName[FOLLOWER_NAME_MAX + 8];
  memset(longName, 'b', sizeof(longName) - 1);
  longName[sizeof(longName) - 1] = '\0';
  TEST_ASSERT_FALSE(followerMembershipEncode(longName, "10.0.0.9", 0, blob));
}

static void test_empty_host_rejects_at_encode() {
  // A membership without a reachable leader host is not a membership
  // (mirrors the v2 follower's leaderHost sentinel rule).
  uint8_t blob[FOLLOWER_MEMBERSHIP_BLOB_LEN];
  TEST_ASSERT_FALSE(followerMembershipEncode("leader", "", 0, blob));
}

static void test_clear_makes_blob_undecodable() {
  uint8_t blob[FOLLOWER_MEMBERSHIP_BLOB_LEN];
  followerMembershipEncode("leader", "10.0.0.9", 1, blob);
  followerMembershipClear(blob);
  char name[FOLLOWER_NAME_MAX + 1];
  char host[FOLLOWER_HOST_MAX + 1];
  uint8_t row = 0;
  TEST_ASSERT_FALSE(followerMembershipDecode(blob, name, host, row));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_membership_round_trips);
  RUN_TEST(test_blank_eeprom_decodes_to_no_membership);
  RUN_TEST(test_corrupt_checksum_rejects);
  RUN_TEST(test_oversized_fields_reject_at_encode);
  RUN_TEST(test_empty_host_rejects_at_encode);
  RUN_TEST(test_clear_makes_blob_undecodable);
  return UNITY_END();
}
