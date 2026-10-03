// Host-side tests for the follower's reset-history ring (FollowerResetLog.h,
// #503): the newest boot leads, older ones age out, and RTC garbage starts a
// fresh ring instead of being reported as history.

#include <cstring>

#include <unity.h>

#include "../../FollowerResetLog.h"

void setUp() {}
void tearDown() {}

static void test_garbage_rtc_starts_a_fresh_ring() {
  FollowerResetLogBlob b;
  memset(&b, 0xFF, sizeof(b));  // power-cycle garbage
  TEST_ASSERT_FALSE(followerResetLogValid(b));
  TEST_ASSERT_EQUAL_INT(0, followerResetLogCount(b));
  followerResetLogPush(b, 6, 0, 0, 0);
  TEST_ASSERT_TRUE(followerResetLogValid(b));
  TEST_ASSERT_EQUAL_INT(1, followerResetLogCount(b));
  TEST_ASSERT_EQUAL_UINT32(6, b.e[0].reasonCause);
  TEST_ASSERT_EQUAL_UINT32(0, b.e[1].reasonCause);  // no garbage carried along
  memset(&b, 0, sizeof(b));
  TEST_ASSERT_FALSE(followerResetLogValid(b));
}

static void test_newest_boot_leads_and_older_ones_age_out() {
  FollowerResetLogBlob b;
  followerResetLogPush(b, 6, 0, 0, 0);                        // power-on
  followerResetLogPush(b, 2, 28, 0x40201234UL, 0x00000004UL); // exception
  followerResetLogPush(b, 4, 0, 0, 0);                        // operator reboot
  TEST_ASSERT_EQUAL_INT(3, followerResetLogCount(b));
  TEST_ASSERT_EQUAL_UINT32(4, b.e[0].reasonCause);
  // The crash an ordinary reboot would have erased is still there.
  TEST_ASSERT_EQUAL_UINT32(2u | (28u << 8), b.e[1].reasonCause);
  TEST_ASSERT_EQUAL_HEX32(0x40201234UL, b.e[1].epc1);
  TEST_ASSERT_EQUAL_HEX32(0x00000004UL, b.e[1].excvaddr);
  for (int i = 0; i < 5; i++) followerResetLogPush(b, 3, 0, 0x40100000UL + i, 0);
  TEST_ASSERT_EQUAL_INT(FOLLOWER_RESETLOG_ENTRIES, followerResetLogCount(b));
  TEST_ASSERT_EQUAL_UINT32(8, b.boots);
  TEST_ASSERT_EQUAL_HEX32(0x40100004UL, b.e[0].epc1);
  TEST_ASSERT_EQUAL_HEX32(0x40100001UL, b.e[3].epc1);
}

static void test_a_flipped_bit_invalidates_the_ring() {
  FollowerResetLogBlob b;
  followerResetLogPush(b, 2, 9, 0x40205678UL, 0x3FFE0001UL);
  uint8_t* raw = (uint8_t*)&b;
  for (size_t i = 0; i < sizeof(b); i++) {
    raw[i] ^= 0x10;
    TEST_ASSERT_FALSE(followerResetLogValid(b));
    raw[i] ^= 0x10;
  }
  TEST_ASSERT_TRUE(followerResetLogValid(b));
}

static void test_record_fits_its_rtc_window() {
  // 15 words from offset 40, inside the 128-word user area and clear of
  // FollowerRescue's 3 words at 32.
  TEST_ASSERT_EQUAL_size_t(15 * 4, sizeof(FollowerResetLogBlob));
  TEST_ASSERT_TRUE(FOLLOWER_RESETLOG_RTC_OFFSET >= 32 + 3);
  TEST_ASSERT_TRUE(FOLLOWER_RESETLOG_RTC_OFFSET + 15 <= 128);
}

static void test_entry_format() {
  FollowerResetEntry e;
  e.reasonCause = 2u | (28u << 8);
  e.epc1 = 0x40201234UL;
  e.excvaddr = 4;
  char buf[32];
  int n = followerResetEntryFormat(e, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("2:28:40201234:00000004", buf);
  TEST_ASSERT_TRUE(n > 0 && n < (int)sizeof(buf));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_garbage_rtc_starts_a_fresh_ring);
  RUN_TEST(test_newest_boot_leads_and_older_ones_age_out);
  RUN_TEST(test_a_flipped_bit_invalidates_the_ring);
  RUN_TEST(test_record_fits_its_rtc_window);
  RUN_TEST(test_entry_format);
  return UNITY_END();
}
