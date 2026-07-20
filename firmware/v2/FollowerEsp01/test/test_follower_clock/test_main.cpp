// Host-side tests for the local clock fallback (#342) — HH:MM composition
// and the phase/tz/sync eligibility gate.

#include <unity.h>

#include "../../FollowerClock.h"

void setUp() {}
void tearDown() {}

static void test_clock_centers_on_row_width() {
  char out[17];
  followerClockText(9, 5, 8, out);
  TEST_ASSERT_EQUAL_STRING(" 09:05  ", out);
  followerClockText(23, 59, 16, out);
  TEST_ASSERT_EQUAL_STRING("     23:59      ", out);
  followerClockText(12, 30, 5, out);
  TEST_ASSERT_EQUAL_STRING("12:30", out);
}

static void test_clock_truncates_on_tiny_width() {
  char out[9];
  followerClockText(12, 34, 3, out);
  TEST_ASSERT_EQUAL_STRING("12:", out);
  followerClockText(12, 34, 0, out);
  TEST_ASSERT_EQUAL_STRING("", out);
}

static void test_eligibility_needs_blank_membership_tz_and_sync() {
  TEST_ASSERT_TRUE(
      followerClockEligible(FollowerPhase::Blank, true, true, true));
  // Grace still HOLDS the leader's last text — never overdraw it.
  TEST_ASSERT_FALSE(
      followerClockEligible(FollowerPhase::Grace, true, true, true));
  // Standalone has no membership and no zone to trust.
  TEST_ASSERT_FALSE(
      followerClockEligible(FollowerPhase::Standalone, false, true, true));
  TEST_ASSERT_FALSE(
      followerClockEligible(FollowerPhase::Blank, false, true, true));
  TEST_ASSERT_FALSE(
      followerClockEligible(FollowerPhase::Blank, true, false, true));
  // Unsynced SNTP = today's blank behavior.
  TEST_ASSERT_FALSE(
      followerClockEligible(FollowerPhase::Blank, true, true, false));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_clock_centers_on_row_width);
  RUN_TEST(test_clock_truncates_on_tiny_width);
  RUN_TEST(test_eligibility_needs_blank_membership_tz_and_sync);
  return UNITY_END();
}
