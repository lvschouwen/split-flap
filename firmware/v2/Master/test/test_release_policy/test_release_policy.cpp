// Native tests for ReleasePolicy.h: when the master looks for a release and
// what it then knows.
#include <unity.h>

#include "../../ReleasePolicy.h"
#include "../../SettingsValidation.h"

void setUp() {}
void tearDown() {}

static void test_the_first_look_waits_for_the_clock_and_two_minutes() {
  ReleaseSchedule s;
  TEST_ASSERT_FALSE(s.due(RELEASE_LOOK_FIRST_MS - 1, true, true));
  TEST_ASSERT_FALSE(s.due(RELEASE_LOOK_FIRST_MS, false, true));
  TEST_ASSERT_TRUE(s.due(RELEASE_LOOK_FIRST_MS, true, true));
  // A clock that is set an hour late: then.
  TEST_ASSERT_TRUE(s.due(3600000UL, true, true));
}

static void test_the_setting_turns_the_look_off() {
  ReleaseSchedule s;
  TEST_ASSERT_FALSE(s.due(RELEASE_LOOK_FIRST_MS, true, false));
  s.done(1000, true);
  TEST_ASSERT_FALSE(s.due(1000 + RELEASE_LOOK_EVERY_MS, true, false));
}

static void test_after_a_look_the_next_is_a_day_later() {
  ReleaseSchedule s;
  s.done(200000, true);
  TEST_ASSERT_FALSE(s.due(200000 + RELEASE_LOOK_EVERY_MS - 1, true, true));
  TEST_ASSERT_TRUE(s.due(200000 + RELEASE_LOOK_EVERY_MS, true, true));
}

static void test_after_a_failed_look_the_next_is_an_hour_later() {
  ReleaseSchedule s;
  s.done(200000, false);
  TEST_ASSERT_FALSE(s.due(200000 + RELEASE_LOOK_RETRY_MS - 1, true, true));
  TEST_ASSERT_TRUE(s.due(200000 + RELEASE_LOOK_RETRY_MS, true, true));
  // One that then goes through is a day again.
  s.done(200000 + RELEASE_LOOK_RETRY_MS, true);
  TEST_ASSERT_FALSE(s.due(200000 + 2 * RELEASE_LOOK_RETRY_MS, true, true));
}

static void test_the_day_is_counted_across_the_wrap_of_the_clock() {
  ReleaseSchedule s;
  s.done(0xFFFFF000UL, true);
  TEST_ASSERT_FALSE(s.due(0x00000100UL, true, true));
  TEST_ASSERT_TRUE(s.due(0xFFFFF000UL + RELEASE_LOOK_EVERY_MS, true, true));
}

static void test_another_channel_is_looked_at_afresh() {
  ReleaseSchedule s;
  s.done(500000, true);
  s.reset();
  TEST_ASSERT_TRUE(s.due(500001, true, true));
}

static void test_only_the_two_channels_are_channels() {
  TEST_ASSERT_TRUE(releaseChannelValid("stable"));
  TEST_ASSERT_TRUE(releaseChannelValid("test"));
  TEST_ASSERT_FALSE(releaseChannelValid(""));
  TEST_ASSERT_FALSE(releaseChannelValid("Stable"));
  TEST_ASSERT_FALSE(releaseChannelValid("../test"));
  TEST_ASSERT_TRUE(releaseChannelValid(RELEASE_CHANNEL_DEFAULT));
  // What a setting may hold is what the site has.
  for (const char* v : {"stable", "test", "", "Stable", "releases", "nightly"}) {
    TEST_ASSERT_EQUAL_MESSAGE(releaseChannelValid(v), isValidReleaseChannelValue(v), v);
  }
}

static void test_a_look_folds_into_newer_up_to_date_or_failed() {
  ReleaseStatus status;
  ReleaseManifest found;
  strcpy(found.tag, "v2026.10.11");
  found.commitTime = 2000;
  releaseStatusFold(status, ReleaseError::Ok, found, 1999, 77);
  TEST_ASSERT_TRUE(ReleaseLookState::Newer == status.look);
  TEST_ASSERT_EQUAL_STRING("v2026.10.11", status.release.tag);
  TEST_ASSERT_EQUAL_UINT32(77, status.lookedAtS);
  releaseStatusFold(status, ReleaseError::Ok, found, 2000, 78);
  TEST_ASSERT_TRUE(ReleaseLookState::UpToDate == status.look);
  // A failed look forgets what an earlier one found: nothing is installed
  // from, or offered on, a release that could not be read now.
  releaseStatusFold(status, ReleaseError::Signature, found, 1999, 79);
  TEST_ASSERT_TRUE(ReleaseLookState::Failed == status.look);
  TEST_ASSERT_TRUE(ReleaseError::Signature == status.why);
  TEST_ASSERT_EQUAL_STRING("", status.release.tag);
  TEST_ASSERT_EQUAL_UINT32(79, status.lookedAtS);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_the_first_look_waits_for_the_clock_and_two_minutes);
  RUN_TEST(test_the_setting_turns_the_look_off);
  RUN_TEST(test_after_a_look_the_next_is_a_day_later);
  RUN_TEST(test_after_a_failed_look_the_next_is_an_hour_later);
  RUN_TEST(test_the_day_is_counted_across_the_wrap_of_the_clock);
  RUN_TEST(test_another_channel_is_looked_at_afresh);
  RUN_TEST(test_only_the_two_channels_are_channels);
  RUN_TEST(test_a_look_folds_into_newer_up_to_date_or_failed);
  return UNITY_END();
}
