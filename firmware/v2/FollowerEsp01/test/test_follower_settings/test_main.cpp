// Host-side tests for the row's EEPROM records: the pairing (the master's
// id, address and tz rule, so a restart lands in Grace and dials the master
// again) and the operator preferences behind it.

#include <cstring>
#include <initializer_list>

#include <unity.h>

#include "../../FollowerSettings.h"

void setUp() {}
void tearDown() {}

static bool decode(const uint8_t* blob, char* name, char* host, char* tz) {
  return followerMembershipDecode(blob, name, host, tz);
}

static void test_layout_is_the_one_boards_in_the_field_hold() {
  // Per-device truth: the fields and the preferences record behind them keep
  // their place across images.
  TEST_ASSERT_EQUAL_UINT32(0x53464634UL, FOLLOWER_MEMBERSHIP_MAGIC);
  TEST_ASSERT_EQUAL(46, FOLLOWER_MEMBERSHIP_NAME_OFF);
  TEST_ASSERT_EQUAL(79, FOLLOWER_MEMBERSHIP_HOST_OFF);
  TEST_ASSERT_EQUAL(120, FOLLOWER_MEMBERSHIP_TZ_OFF);
  TEST_ASSERT_EQUAL(186, FOLLOWER_MEMBERSHIP_BLOB_LEN);
  TEST_ASSERT_EQUAL(186, FOLLOWER_PREFS_OFF);
  TEST_ASSERT_EQUAL(189, FOLLOWER_EEPROM_LEN);
}

static void test_pairing_round_trips() {
  uint8_t blob[FOLLOWER_MEMBERSHIP_BLOB_LEN];
  TEST_ASSERT_TRUE(followerMembershipEncode(
      "split-flap-master", "192.168.15.22", "CET-1CEST,M3.5.0,M10.5.0/3", blob));
  char name[FOLLOWER_NAME_MAX + 1];
  char host[FOLLOWER_HOST_MAX + 1];
  char tz[FOLLOWER_TZ_MAX + 1];
  TEST_ASSERT_TRUE(decode(blob, name, host, tz));
  TEST_ASSERT_EQUAL_STRING("split-flap-master", name);
  TEST_ASSERT_EQUAL_STRING("192.168.15.22", host);
  TEST_ASSERT_EQUAL_STRING("CET-1CEST,M3.5.0,M10.5.0/3", tz);
}

static void test_bytes_no_longer_used_are_written_as_zero() {
  uint8_t blob[FOLLOWER_MEMBERSHIP_BLOB_LEN];
  memset(blob, 0xEE, sizeof(blob));
  TEST_ASSERT_TRUE(followerMembershipEncode("m", "10.0.0.9", "", blob));
  for (int i = 4; i < FOLLOWER_MEMBERSHIP_NAME_OFF; i++) {
    TEST_ASSERT_EQUAL_UINT8(0, blob[i]);
  }
}

static void test_a_record_with_those_bytes_set_still_decodes() {
  // What an image that used them left behind: the name, host and tz are
  // where they were, and the checksum covers the whole record.
  uint8_t blob[FOLLOWER_MEMBERSHIP_BLOB_LEN];
  TEST_ASSERT_TRUE(followerMembershipEncode("ldr", "10.0.0.9", "UTC0", blob));
  for (int i = 4; i < FOLLOWER_MEMBERSHIP_NAME_OFF; i++) blob[i] = (uint8_t)(i * 3 + 1);
  blob[FOLLOWER_MEMBERSHIP_BLOB_LEN - 1] = followerMembershipChecksum(blob);
  char name[FOLLOWER_NAME_MAX + 1];
  char host[FOLLOWER_HOST_MAX + 1];
  char tz[FOLLOWER_TZ_MAX + 1];
  TEST_ASSERT_TRUE(decode(blob, name, host, tz));
  TEST_ASSERT_EQUAL_STRING("ldr", name);
  TEST_ASSERT_EQUAL_STRING("10.0.0.9", host);
  TEST_ASSERT_EQUAL_STRING("UTC0", tz);
}

static void test_blank_eeprom_decodes_to_no_pairing() {
  uint8_t blob[FOLLOWER_MEMBERSHIP_BLOB_LEN];
  char name[FOLLOWER_NAME_MAX + 1];
  char host[FOLLOWER_HOST_MAX + 1];
  char tz[FOLLOWER_TZ_MAX + 1];
  memset(blob, 0xFF, sizeof(blob));  // factory-fresh flash
  TEST_ASSERT_FALSE(decode(blob, name, host, tz));
  memset(blob, 0x00, sizeof(blob));
  TEST_ASSERT_FALSE(decode(blob, name, host, tz));
}

static void test_corrupt_byte_rejects() {
  char name[FOLLOWER_NAME_MAX + 1];
  char host[FOLLOWER_HOST_MAX + 1];
  char tz[FOLLOWER_TZ_MAX + 1];
  for (int at : {10, FOLLOWER_MEMBERSHIP_NAME_OFF, FOLLOWER_MEMBERSHIP_TZ_OFF + 2}) {
    uint8_t blob[FOLLOWER_MEMBERSHIP_BLOB_LEN];
    followerMembershipEncode("master", "10.0.0.9", "UTC0", blob);
    blob[at] ^= 0x40;
    TEST_ASSERT_FALSE(decode(blob, name, host, tz));
  }
}

static void test_oversized_fields_reject_at_encode() {
  uint8_t blob[FOLLOWER_MEMBERSHIP_BLOB_LEN];
  char longHost[FOLLOWER_HOST_MAX + 8];
  memset(longHost, 'a', sizeof(longHost) - 1);
  longHost[sizeof(longHost) - 1] = '\0';
  TEST_ASSERT_FALSE(followerMembershipEncode("n", longHost, "", blob));
  char longName[FOLLOWER_NAME_MAX + 8];
  memset(longName, 'b', sizeof(longName) - 1);
  longName[sizeof(longName) - 1] = '\0';
  TEST_ASSERT_FALSE(followerMembershipEncode(longName, "10.0.0.9", "", blob));
  char longTz[FOLLOWER_TZ_MAX + 2];
  memset(longTz, 'X', sizeof(longTz) - 1);
  longTz[sizeof(longTz) - 1] = '\0';
  TEST_ASSERT_FALSE(followerMembershipEncode("n", "10.0.0.9", longTz, blob));
}

static void test_empty_host_rejects_at_encode() {
  // A pairing without an address to dial is not a pairing.
  uint8_t blob[FOLLOWER_MEMBERSHIP_BLOB_LEN];
  TEST_ASSERT_FALSE(followerMembershipEncode("master", "", "", blob));
}

static void test_clear_makes_blob_undecodable() {
  uint8_t blob[FOLLOWER_MEMBERSHIP_BLOB_LEN];
  followerMembershipEncode("master", "10.0.0.9", "", blob);
  followerMembershipClear(blob);
  char name[FOLLOWER_NAME_MAX + 1];
  char host[FOLLOWER_HOST_MAX + 1];
  char tz[FOLLOWER_TZ_MAX + 1];
  TEST_ASSERT_FALSE(decode(blob, name, host, tz));
}

// --- operator preferences (#513) ---

static void test_prefs_roundtrip_both_values() {
  for (int on = 0; on <= 1; on++) {
    FollowerPrefs in;
    in.reflashOnBoot = on != 0;
    uint8_t rec[FOLLOWER_PREFS_LEN];
    followerPrefsEncode(in, rec);
    TEST_ASSERT_EQUAL(in.reflashOnBoot, followerPrefsDecode(rec).reflashOnBoot);
  }
}

static void test_prefs_unwritten_eeprom_means_defaults() {
  // What a firmware predating the record leaves behind the pairing.
  uint8_t erased[FOLLOWER_PREFS_LEN] = {0xFF, 0xFF, 0xFF};
  TEST_ASSERT_TRUE(followerPrefsDecode(erased).reflashOnBoot);
  uint8_t zeroed[FOLLOWER_PREFS_LEN] = {0, 0, 0};
  TEST_ASSERT_TRUE(followerPrefsDecode(zeroed).reflashOnBoot);
}

static void test_prefs_corruption_never_decodes_to_off() {
  FollowerPrefs off;
  off.reflashOnBoot = false;
  uint8_t rec[FOLLOWER_PREFS_LEN];
  followerPrefsEncode(off, rec);
  TEST_ASSERT_FALSE(followerPrefsDecode(rec).reflashOnBoot);
  for (int i = 0; i < FOLLOWER_PREFS_LEN; i++) {
    for (int bit = 0; bit < 8; bit++) {
      rec[i] ^= (uint8_t)(1 << bit);
      TEST_ASSERT_TRUE(followerPrefsDecode(rec).reflashOnBoot);
      rec[i] ^= (uint8_t)(1 << bit);
    }
  }
}

static void test_prefs_keep_the_fallback_beside_the_boot_brake() {
  const FollowerFallback all[] = {FollowerFallback::Blank, FollowerFallback::Time,
                                  FollowerFallback::Date};
  for (FollowerFallback f : all) {
    for (int on = 0; on <= 1; on++) {
      FollowerPrefs in;
      in.fallback = f;
      in.reflashOnBoot = on != 0;
      uint8_t rec[FOLLOWER_PREFS_LEN];
      followerPrefsEncode(in, rec);
      FollowerPrefs back = followerPrefsDecode(rec);
      TEST_ASSERT_EQUAL((int)f, (int)back.fallback);
      TEST_ASSERT_EQUAL(in.reflashOnBoot, back.reflashOnBoot);
    }
  }
}

static void test_a_record_from_before_the_fallback_field_shows_the_time() {
  // What a build without the field wrote: magic, the boot-brake bit, check.
  for (uint8_t flags = 0; flags <= 1; flags++) {
    uint8_t rec[FOLLOWER_PREFS_LEN] = {FOLLOWER_PREFS_MAGIC, flags, 0};
    rec[2] = followerPrefsCheck(rec);
    FollowerPrefs p = followerPrefsDecode(rec);
    TEST_ASSERT_EQUAL((int)FollowerFallback::Time, (int)p.fallback);
    TEST_ASSERT_EQUAL(flags != 0, p.reflashOnBoot);
  }
  uint8_t erased[FOLLOWER_PREFS_LEN] = {0xFF, 0xFF, 0xFF};
  TEST_ASSERT_EQUAL((int)FollowerFallback::Time, (int)followerPrefsDecode(erased).fallback);
}

static void test_prefs_sit_behind_the_membership_blob() {
  TEST_ASSERT_EQUAL_INT(FOLLOWER_MEMBERSHIP_BLOB_LEN, FOLLOWER_PREFS_OFF);
  TEST_ASSERT_EQUAL_INT(FOLLOWER_MEMBERSHIP_BLOB_LEN + FOLLOWER_PREFS_LEN,
                        FOLLOWER_EEPROM_LEN);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_layout_is_the_one_boards_in_the_field_hold);
  RUN_TEST(test_pairing_round_trips);
  RUN_TEST(test_bytes_no_longer_used_are_written_as_zero);
  RUN_TEST(test_a_record_with_those_bytes_set_still_decodes);
  RUN_TEST(test_blank_eeprom_decodes_to_no_pairing);
  RUN_TEST(test_corrupt_byte_rejects);
  RUN_TEST(test_oversized_fields_reject_at_encode);
  RUN_TEST(test_empty_host_rejects_at_encode);
  RUN_TEST(test_clear_makes_blob_undecodable);
  RUN_TEST(test_prefs_roundtrip_both_values);
  RUN_TEST(test_prefs_unwritten_eeprom_means_defaults);
  RUN_TEST(test_prefs_corruption_never_decodes_to_off);
  RUN_TEST(test_prefs_keep_the_fallback_beside_the_boot_brake);
  RUN_TEST(test_a_record_from_before_the_fallback_field_shows_the_time);
  RUN_TEST(test_prefs_sit_behind_the_membership_blob);
  return UNITY_END();
}
