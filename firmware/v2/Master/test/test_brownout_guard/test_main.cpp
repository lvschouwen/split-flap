// Native tests for BrownoutGuardPolicy.h: brownouts in a row that each cut a
// young boot short hold the next boot; one brownout (a power cycle) never does.
#include <unity.h>

#include "BrownoutGuardPolicy.h"

void setUp() {}
void tearDown() {}

static const uint32_t kWindow = BROWNOUT_LOOP_WINDOW_MS;

// One boot that browns out before it lived the window.
static BrownoutGuardRecord youngBrownout(const BrownoutGuardRecord& prev) {
  return brownoutGuardStart(prev, CRASH_RESET_BROWNOUT);
}

static void test_a_single_brownout_after_a_long_run_is_a_power_cycle() {
  BrownoutGuardRecord r = brownoutGuardStart(BrownoutGuardRecord{}, CRASH_RESET_POWERON);
  TEST_ASSERT_TRUE(brownoutGuardSettle(r, kWindow));
  r = brownoutGuardStart(r, CRASH_RESET_BROWNOUT);
  TEST_ASSERT_EQUAL(0, r.streak);
  TEST_ASSERT_FALSE(r.held);
  TEST_ASSERT_TRUE(r.young);
}

static void test_the_limit_of_young_brownouts_holds_and_not_before() {
  BrownoutGuardRecord r = brownoutGuardStart(BrownoutGuardRecord{}, CRASH_RESET_POWERON);
  for (int i = 1; i < BROWNOUT_LOOP_LIMIT; i++) {
    r = youngBrownout(r);
    TEST_ASSERT_EQUAL(i, r.streak);
    TEST_ASSERT_FALSE(r.held);
  }
  r = youngBrownout(r);
  TEST_ASSERT_EQUAL(BROWNOUT_LOOP_LIMIT, r.streak);
  TEST_ASSERT_TRUE(r.held);
}

static void test_another_reset_ends_the_streak() {
  BrownoutGuardRecord r = brownoutGuardStart(BrownoutGuardRecord{}, CRASH_RESET_POWERON);
  r = youngBrownout(r);
  r = youngBrownout(r);
  r = brownoutGuardStart(r, CRASH_RESET_SW);
  TEST_ASSERT_EQUAL(0, r.streak);
  r = youngBrownout(r);
  TEST_ASSERT_EQUAL(1, r.streak);
  TEST_ASSERT_FALSE(r.held);
}

static void test_a_boot_that_lived_the_window_ends_the_streak() {
  BrownoutGuardRecord r = brownoutGuardStart(BrownoutGuardRecord{}, CRASH_RESET_POWERON);
  r = youngBrownout(r);
  r = youngBrownout(r);
  TEST_ASSERT_FALSE(brownoutGuardSettle(r, kWindow - 1));
  TEST_ASSERT_TRUE(brownoutGuardSettle(r, kWindow));
  TEST_ASSERT_FALSE(brownoutGuardSettle(r, kWindow + 1));  // once
  r = youngBrownout(r);
  TEST_ASSERT_EQUAL(0, r.streak);
  TEST_ASSERT_FALSE(r.held);
}

static BrownoutGuardRecord held() {
  BrownoutGuardRecord r = brownoutGuardStart(BrownoutGuardRecord{}, CRASH_RESET_POWERON);
  for (int i = 0; i < BROWNOUT_LOOP_LIMIT; i++) r = youngBrownout(r);
  return r;
}

static void test_the_hold_is_latched_across_every_reset() {
  BrownoutGuardRecord r = held();
  TEST_ASSERT_FALSE(brownoutGuardSettle(r, 10 * kWindow));  // never by itself
  r = brownoutGuardStart(r, CRASH_RESET_SW);
  TEST_ASSERT_TRUE(r.held);
  r = brownoutGuardStart(r, CRASH_RESET_POWERON);
  TEST_ASSERT_TRUE(r.held);
}

static void test_a_clear_ends_hold_and_streak() {
  BrownoutGuardRecord r = held();
  TEST_ASSERT_TRUE(brownoutGuardClear(r));
  TEST_ASSERT_FALSE(r.held);
  TEST_ASSERT_FALSE(brownoutGuardClear(r));  // nothing left to clear
  // The cleared boot counts as one that lived: a brownout right after starts over.
  r = youngBrownout(r);
  TEST_ASSERT_EQUAL(0, r.streak);
  TEST_ASSERT_FALSE(r.held);
}

static void test_clear_without_a_hold_changes_nothing() {
  BrownoutGuardRecord r = youngBrownout(brownoutGuardStart(BrownoutGuardRecord{}, 1));
  const uint8_t streak = r.streak;
  TEST_ASSERT_FALSE(brownoutGuardClear(r));
  TEST_ASSERT_EQUAL(streak, r.streak);
  TEST_ASSERT_TRUE(r.young);
}

static void test_record_round_trips() {
  BrownoutGuardRecord r = held();
  uint8_t blob[BROWNOUT_GUARD_BLOB_LEN];
  brownoutGuardEncode(r, blob);
  BrownoutGuardRecord back;
  TEST_ASSERT_TRUE(brownoutGuardDecode(blob, sizeof(blob), back));
  TEST_ASSERT_EQUAL(r.streak, back.streak);
  TEST_ASSERT_EQUAL(r.young, back.young);
  TEST_ASSERT_EQUAL(r.held, back.held);
}

static void test_garbage_never_decodes_as_a_hold() {
  uint8_t blob[BROWNOUT_GUARD_BLOB_LEN] = {0xFF, 0xFF, 0xFF, 0xFF};
  BrownoutGuardRecord r;
  TEST_ASSERT_FALSE(brownoutGuardDecode(blob, sizeof(blob), r));
  TEST_ASSERT_FALSE(r.held);
  brownoutGuardEncode(held(), blob);
  blob[2] ^= BROWNOUT_GUARD_FLAG_HELD;  // a flipped bit fails the check
  TEST_ASSERT_FALSE(brownoutGuardDecode(blob, sizeof(blob), r));
  TEST_ASSERT_FALSE(brownoutGuardDecode(blob, BROWNOUT_GUARD_BLOB_LEN - 1, r));
  TEST_ASSERT_FALSE(brownoutGuardDecode(nullptr, BROWNOUT_GUARD_BLOB_LEN, r));
}

static void test_a_first_boot_with_no_record_starts_clean() {
  // The first boot of this feature finds no record, even after a brownout.
  BrownoutGuardRecord r = brownoutGuardStart(BrownoutGuardRecord{}, CRASH_RESET_BROWNOUT);
  TEST_ASSERT_EQUAL(0, r.streak);
  TEST_ASSERT_FALSE(r.held);
}

static void test_a_held_boot_releases_itself_after_the_release_time() {
  BrownoutGuardRecord r = held();
  TEST_ASSERT_FALSE(brownoutGuardAutoRelease(r, BROWNOUT_HOLD_RELEASE_MS - 1));  // 29:59.999
  TEST_ASSERT_TRUE(r.held);
  TEST_ASSERT_TRUE(brownoutGuardAutoRelease(r, BROWNOUT_HOLD_RELEASE_MS));  // 30:00
  TEST_ASSERT_FALSE(r.held);
  TEST_ASSERT_EQUAL(0, r.streak);
  TEST_ASSERT_FALSE(r.young);  // counts as a boot that lived
  TEST_ASSERT_FALSE(brownoutGuardAutoRelease(r, BROWNOUT_HOLD_RELEASE_MS + 1));  // once
  r = youngBrownout(r);  // a brownout after the release starts a new count
  TEST_ASSERT_EQUAL(0, r.streak);
  TEST_ASSERT_FALSE(r.held);
}

static void test_a_brownout_before_the_release_keeps_the_hold_and_restarts_its_time() {
  BrownoutGuardRecord r = held();
  TEST_ASSERT_FALSE(brownoutGuardAutoRelease(r, BROWNOUT_HOLD_RELEASE_MS - 1));
  r = youngBrownout(r);  // browned out at 29:59: the next boot is still held
  TEST_ASSERT_TRUE(r.held);
  // Its own 30 min, from its own start.
  TEST_ASSERT_FALSE(brownoutGuardAutoRelease(r, 60UL * 1000UL));
  TEST_ASSERT_TRUE(r.held);
  TEST_ASSERT_TRUE(brownoutGuardAutoRelease(r, BROWNOUT_HOLD_RELEASE_MS));
  TEST_ASSERT_FALSE(r.held);
}

static void test_auto_release_never_touches_a_boot_that_is_not_held() {
  BrownoutGuardRecord r = youngBrownout(brownoutGuardStart(BrownoutGuardRecord{}, 1));
  TEST_ASSERT_FALSE(brownoutGuardAutoRelease(r, 2 * BROWNOUT_HOLD_RELEASE_MS));
  TEST_ASSERT_EQUAL(1, r.streak);
  TEST_ASSERT_TRUE(r.young);
  TEST_ASSERT_TRUE(BROWNOUT_HOLD_RELEASE_MS > BROWNOUT_LOOP_WINDOW_MS);
}

static void test_the_window_outlasts_the_observed_loop() {
  // #504: three failed boots inside 31 s; the boot-home of 16 units ~1 min.
  TEST_ASSERT_TRUE(BROWNOUT_LOOP_WINDOW_MS >= 5UL * 60UL * 1000UL);
  TEST_ASSERT_TRUE(BROWNOUT_LOOP_LIMIT >= 3);  // two can be an operator
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_a_single_brownout_after_a_long_run_is_a_power_cycle);
  RUN_TEST(test_the_limit_of_young_brownouts_holds_and_not_before);
  RUN_TEST(test_another_reset_ends_the_streak);
  RUN_TEST(test_a_boot_that_lived_the_window_ends_the_streak);
  RUN_TEST(test_the_hold_is_latched_across_every_reset);
  RUN_TEST(test_a_clear_ends_hold_and_streak);
  RUN_TEST(test_clear_without_a_hold_changes_nothing);
  RUN_TEST(test_record_round_trips);
  RUN_TEST(test_garbage_never_decodes_as_a_hold);
  RUN_TEST(test_a_first_boot_with_no_record_starts_clean);
  RUN_TEST(test_a_held_boot_releases_itself_after_the_release_time);
  RUN_TEST(test_a_brownout_before_the_release_keeps_the_hold_and_restarts_its_time);
  RUN_TEST(test_auto_release_never_touches_a_boot_that_is_not_held);
  RUN_TEST(test_the_window_outlasts_the_observed_loop);
  return UNITY_END();
}
