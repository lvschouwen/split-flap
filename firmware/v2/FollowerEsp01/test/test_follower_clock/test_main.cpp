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

static void test_date_is_the_masters_shape_where_it_fits() {
  char out[17];
  followerDateText(5, 10, 26, 16, out);
  TEST_ASSERT_EQUAL_STRING("   05 OCT 26    ", out);
  followerDateText(31, 12, 99, 9, out);
  TEST_ASSERT_EQUAL_STRING("31 DEC 99", out);
  followerDateText(1, 1, 0, 10, out);
  TEST_ASSERT_EQUAL_STRING("01 JAN 00 ", out);
}

static void test_date_on_a_narrow_row_is_day_and_month() {
  char out[9];
  followerDateText(5, 10, 26, 8, out);
  TEST_ASSERT_EQUAL_STRING(" 05-10  ", out);
  followerDateText(5, 10, 26, 5, out);
  TEST_ASSERT_EQUAL_STRING("05-10", out);
  followerDateText(5, 10, 26, 3, out);
  TEST_ASSERT_EQUAL_STRING("05-", out);
  followerDateText(5, 10, 26, 0, out);
  TEST_ASSERT_EQUAL_STRING("", out);
}

static void test_a_row_set_to_blank_never_shows_the_clock() {
  TEST_ASSERT_TRUE(
      followerClockEligible(ClusterFollowerPhase::LeaderLost, true, true, true, true));
  TEST_ASSERT_FALSE(
      followerClockEligible(ClusterFollowerPhase::LeaderLost, true, true, true, false));
}

static void test_eligibility_needs_blank_membership_tz_and_sync() {
  TEST_ASSERT_TRUE(
      followerClockEligible(ClusterFollowerPhase::LeaderLost, true, true, true));
  // Grace still HOLDS the leader's last text — never overdraw it.
  TEST_ASSERT_FALSE(
      followerClockEligible(ClusterFollowerPhase::Grace, true, true, true));
  // Standalone has no membership and no zone to trust.
  TEST_ASSERT_FALSE(
      followerClockEligible(ClusterFollowerPhase::Standalone, false, true, true));
  TEST_ASSERT_FALSE(
      followerClockEligible(ClusterFollowerPhase::LeaderLost, false, true, true));
  TEST_ASSERT_FALSE(
      followerClockEligible(ClusterFollowerPhase::LeaderLost, true, false, true));
  // Unsynced SNTP = today's blank behavior.
  TEST_ASSERT_FALSE(
      followerClockEligible(ClusterFollowerPhase::LeaderLost, true, true, false));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_clock_centers_on_row_width);
  RUN_TEST(test_clock_truncates_on_tiny_width);
  RUN_TEST(test_date_is_the_masters_shape_where_it_fits);
  RUN_TEST(test_date_on_a_narrow_row_is_day_and_month);
  RUN_TEST(test_a_row_set_to_blank_never_shows_the_clock);
  RUN_TEST(test_eligibility_needs_blank_membership_tz_and_sync);
  return UNITY_END();
}
