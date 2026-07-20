// Host-side tests for the boot-rescue beacon policy (#343) — RTC blob
// encode/decode armor, the consecutive-bad-boot threshold, and counter
// saturation.

#include <unity.h>

#include "../../FollowerRescue.h"

void setUp() {}
void tearDown() {}

static void test_roundtrip_counts() {
  FollowerRescueBlob b;
  followerRescueEncode(b, 2);
  TEST_ASSERT_EQUAL_UINT32(2, followerRescueDecode(b));
}

static void test_factory_garbage_decodes_as_zero() {
  // Power-up RTC is garbage; all-FF and all-00 fills are the classic
  // shapes and must read as "healthy, no history".
  FollowerRescueBlob b;
  b.magic = 0xFFFFFFFFUL;
  b.counter = 0xFFFFFFFFUL;
  b.check = 0xFFFFFFFFUL;
  TEST_ASSERT_EQUAL_UINT32(0, followerRescueDecode(b));
  b.magic = 0;
  b.counter = 0;
  b.check = 0;
  TEST_ASSERT_EQUAL_UINT32(0, followerRescueDecode(b));
}

static void test_corrupted_counter_fails_check() {
  FollowerRescueBlob b;
  followerRescueEncode(b, 1);
  b.counter = 200;  // torn write: counter moved, check didn't
  TEST_ASSERT_EQUAL_UINT32(0, followerRescueDecode(b));
}

static void test_beacon_engages_at_cap() {
  TEST_ASSERT_FALSE(followerRescueBeaconAtBoot(0));
  TEST_ASSERT_FALSE(followerRescueBeaconAtBoot(FOLLOWER_RESCUE_BOOT_CAP - 1));
  TEST_ASSERT_TRUE(followerRescueBeaconAtBoot(FOLLOWER_RESCUE_BOOT_CAP));
  TEST_ASSERT_TRUE(followerRescueBeaconAtBoot(FOLLOWER_RESCUE_BOOT_CAP + 5));
}

static void test_counter_saturates() {
  TEST_ASSERT_EQUAL_UINT32(1, followerRescueNextCounter(0));
  TEST_ASSERT_EQUAL_UINT32(0xFFFFFFFFUL,
                           followerRescueNextCounter(0xFFFFFFFEUL));
  TEST_ASSERT_EQUAL_UINT32(0xFFFFFFFFUL,
                           followerRescueNextCounter(0xFFFFFFFFUL));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_roundtrip_counts);
  RUN_TEST(test_factory_garbage_decodes_as_zero);
  RUN_TEST(test_corrupted_counter_fails_check);
  RUN_TEST(test_beacon_engages_at_cap);
  RUN_TEST(test_counter_saturates);
  return UNITY_END();
}
