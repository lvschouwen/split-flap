// Native tests for WallUpdatePolicy.h: when the master offers the stored row
// image, and what it makes of the answers.
#include <unity.h>

#include "WallUpdatePolicy.h"

static const char STORED[] = "new1234";

static WallUpdater up;
static WallUpdateRow rows[3];

void setUp() {
  up = WallUpdater{};
  for (WallUpdateRow& r : rows) r = WallUpdateRow{true, false, "old0000"};
}
void tearDown() {}

static wl_UpdateState state(wl_UpdatePhase phase, wl_UpdateReason reason = wl_UpdateReason_UPDATE_REASON_NONE,
                            const char* rev = STORED) {
  wl_UpdateState s = wl_UpdateState_init_zero;
  strcpy(s.rev, rev);
  s.phase = phase;
  s.reason = reason;
  return s;
}

static int candidate(uint32_t now) { return up.nextCandidate(rows, 3, STORED, now); }

// An offer to `row` that the row reports as failed.
static void failOnce(int row, uint32_t now) {
  up.offered(row, false, now);
  TEST_ASSERT_TRUE(WallUpdateEnd::Failed ==
                   up.answer(row, state(wl_UpdatePhase_UPDATE_FAILED), STORED, now + 100));
}

// ---- who is offered ------------------------------------------------------------

static void test_the_first_row_on_another_rev_is_offered_in_either_direction() {
  rows[0].rev = STORED;
  rows[1].rev = "zzz9999";  // "newer" than the stored one: still not what the wall runs
  TEST_ASSERT_EQUAL(1, candidate(0));
}

static void test_nothing_is_offered_without_a_stored_image() {
  TEST_ASSERT_EQUAL(-1, up.nextCandidate(rows, 3, "", 0));
  TEST_ASSERT_EQUAL(-1, up.nextCandidate(rows, 3, nullptr, 0));
}

static void test_a_row_that_cannot_take_it_now_or_never_said_its_rev_is_passed_over() {
  rows[0].reachable = false;
  rows[1].rev = "";
  TEST_ASSERT_EQUAL(2, candidate(0));
  rows[2].rev = STORED;
  TEST_ASSERT_EQUAL(-1, candidate(0));
}

static void test_a_row_in_rescue_mode_is_offered_the_rev_it_reports() {
  for (WallUpdateRow& r : rows) r.rev = STORED;
  TEST_ASSERT_EQUAL(-1, candidate(0));
  rows[1].rescue = true;
  TEST_ASSERT_EQUAL(1, candidate(0));
}

static void test_one_row_at_a_time() {
  up.offered(0, false, 1000);
  TEST_ASSERT_EQUAL(-1, candidate(1500));
  up.answer(0, state(wl_UpdatePhase_UPDATE_DOWNLOADING), STORED, 2000);
  TEST_ASSERT_EQUAL(-1, candidate(2500));
  up.answer(0, state(wl_UpdatePhase_UPDATE_INSTALLED), STORED, 30000);
  TEST_ASSERT_TRUE(WallUpdatePhase::Returning == up.phase);
  TEST_ASSERT_EQUAL(-1, candidate(31000));
}

// ---- the good path -------------------------------------------------------------

static void test_a_row_back_on_the_stored_rev_has_converged_and_the_next_follows_at_once() {
  up.offered(0, false, 1000);
  up.answer(0, state(wl_UpdatePhase_UPDATE_DOWNLOADING), STORED, 1500);
  up.answer(0, state(wl_UpdatePhase_UPDATE_INSTALLED), STORED, 25000);
  TEST_ASSERT_TRUE(WallUpdateEnd::Converged == up.hello(0, STORED, false, true, STORED, 40000));
  TEST_ASSERT_TRUE(WallUpdatePhase::Idle == up.phase);
  TEST_ASSERT_EQUAL(0, up.attempts[0]);
  rows[0].rev = STORED;
  TEST_ASSERT_EQUAL(1, candidate(40000));
}

static void test_a_row_that_restarts_before_its_installed_was_read_has_converged_too() {
  // The row restarts right after the install; "installed" then arrives on
  // the new connection, after its Hello.
  up.offered(0, false, 1000);
  up.answer(0, state(wl_UpdatePhase_UPDATE_DOWNLOADING), STORED, 1500);
  TEST_ASSERT_TRUE(WallUpdateEnd::Converged == up.hello(0, STORED, false, true, STORED, 30000));
  TEST_ASSERT_TRUE(WallUpdateEnd::None ==
                   up.answer(0, state(wl_UpdatePhase_UPDATE_INSTALLED), STORED, 30100));
  TEST_ASSERT_TRUE(WallUpdatePhase::Idle == up.phase);
}

static void test_a_redial_on_the_old_boot_id_is_not_the_row_coming_back() {
  up.offered(0, false, 1000);
  up.answer(0, state(wl_UpdatePhase_UPDATE_DOWNLOADING), STORED, 1500);
  TEST_ASSERT_TRUE(WallUpdateEnd::None == up.hello(0, "old0000", false, false, STORED, 9000));
  TEST_ASSERT_TRUE(WallUpdatePhase::Downloading == up.phase);
}

// ---- what costs an attempt -----------------------------------------------------

static void test_a_failed_download_costs_an_attempt_and_holds_every_offer_off() {
  failOnce(0, 1000);
  TEST_ASSERT_EQUAL(1, up.attempts[0]);
  TEST_ASSERT_FALSE(up.blocked[0]);
  TEST_ASSERT_EQUAL(-1, candidate(1100 + WALL_UPDATE_HOLDOFF_MS - 1));
  TEST_ASSERT_EQUAL(0, candidate(1100 + WALL_UPDATE_HOLDOFF_MS));
}

static void test_three_failed_offers_block_the_row_and_the_next_row_is_served() {
  for (int i = 0; i < WALL_UPDATE_ATTEMPT_CAP; i++) failOnce(0, 100000UL * (i + 1));
  TEST_ASSERT_TRUE(up.blocked[0]);
  TEST_ASSERT_EQUAL(1, candidate(1000000));
}

static void test_a_row_busy_with_its_units_is_asked_again_later_at_no_cost() {
  up.offered(0, false, 1000);
  TEST_ASSERT_TRUE(WallUpdateEnd::HeldOff ==
                   up.answer(0, state(wl_UpdatePhase_UPDATE_REFUSED,
                                      wl_UpdateReason_UPDATE_UNITS_BUSY), STORED, 1100));
  TEST_ASSERT_EQUAL(0, up.attempts[0]);
  TEST_ASSERT_EQUAL(-1, candidate(2000));
  TEST_ASSERT_EQUAL(0, candidate(1100 + WALL_UPDATE_HOLDOFF_MS));
}

static void test_any_other_refusal_costs_an_attempt() {
  up.offered(0, false, 1000);
  TEST_ASSERT_TRUE(WallUpdateEnd::Refused ==
                   up.answer(0, state(wl_UpdatePhase_UPDATE_REFUSED,
                                      wl_UpdateReason_UPDATE_TOO_LARGE), STORED, 1100));
  TEST_ASSERT_EQUAL(1, up.attempts[0]);
}

static void test_an_unanswered_offer_is_made_again_later_at_no_cost() {
  up.offered(0, false, 1000);
  TEST_ASSERT_TRUE(WallUpdateEnd::None == up.tick(1000 + WALL_UPDATE_ANSWER_MS - 1));
  TEST_ASSERT_TRUE(WallUpdateEnd::NoAnswer == up.tick(1000 + WALL_UPDATE_ANSWER_MS));
  TEST_ASSERT_EQUAL(0, up.attempts[0]);
  TEST_ASSERT_TRUE(WallUpdatePhase::Idle == up.phase);
}

static void test_a_download_that_never_reports_its_end_times_out_at_a_cost() {
  up.offered(0, false, 1000);
  up.answer(0, state(wl_UpdatePhase_UPDATE_DOWNLOADING), STORED, 2000);
  TEST_ASSERT_TRUE(WallUpdateEnd::None == up.tick(2000 + WALL_UPDATE_DOWNLOAD_MS - 1));
  TEST_ASSERT_TRUE(WallUpdateEnd::TimedOut == up.tick(2000 + WALL_UPDATE_DOWNLOAD_MS));
  TEST_ASSERT_EQUAL(1, up.attempts[0]);
}

static void test_a_row_that_never_comes_back_times_out_at_a_cost() {
  up.offered(0, false, 1000);
  up.answer(0, state(wl_UpdatePhase_UPDATE_INSTALLED), STORED, 2000);
  TEST_ASSERT_TRUE(WallUpdateEnd::None == up.tick(2000 + WALL_UPDATE_RETURN_MS - 1));
  TEST_ASSERT_TRUE(WallUpdateEnd::TimedOut == up.tick(2000 + WALL_UPDATE_RETURN_MS));
  TEST_ASSERT_EQUAL(1, up.attempts[0]);
}

static void test_a_row_back_on_another_rev_rolled_back_at_a_cost() {
  up.offered(0, false, 1000);
  up.answer(0, state(wl_UpdatePhase_UPDATE_INSTALLED), STORED, 2000);
  TEST_ASSERT_TRUE(WallUpdateEnd::RolledBack == up.hello(0, "old0000", false, true, STORED, 20000));
  TEST_ASSERT_EQUAL(1, up.attempts[0]);
  TEST_ASSERT_EQUAL(-1, candidate(21000));  // held off
}

static void test_an_offer_costs_one_attempt_however_it_goes_wrong() {
  up.offered(0, true, 1000);  // rescue: paid when made
  TEST_ASSERT_EQUAL(1, up.attempts[0]);
  up.answer(0, state(wl_UpdatePhase_UPDATE_FAILED), STORED, 2000);
  TEST_ASSERT_EQUAL(1, up.attempts[0]);
}

static void test_a_result_about_another_image_or_row_or_no_offer_is_history() {
  TEST_ASSERT_TRUE(WallUpdateEnd::None ==
                   up.answer(0, state(wl_UpdatePhase_UPDATE_FAILED), STORED, 500));  // no offer
  up.offered(0, false, 1000);
  TEST_ASSERT_TRUE(WallUpdateEnd::None ==
                   up.answer(1, state(wl_UpdatePhase_UPDATE_FAILED), STORED, 1100));
  TEST_ASSERT_TRUE(WallUpdateEnd::None ==
                   up.answer(0, state(wl_UpdatePhase_UPDATE_FAILED,
                                      wl_UpdateReason_UPDATE_FLASH, "older12"), STORED, 1200));
  TEST_ASSERT_TRUE(WallUpdatePhase::Offered == up.phase);
  TEST_ASSERT_EQUAL(0, up.attempts[0]);
}

// ---- rescue mode ---------------------------------------------------------------

static void test_a_rescue_offer_costs_its_attempt_when_made_and_coming_back_well_keeps_it() {
  up.offered(0, true, 1000);
  TEST_ASSERT_EQUAL(1, up.attempts[0]);
  up.answer(0, state(wl_UpdatePhase_UPDATE_INSTALLED), STORED, 20000);
  TEST_ASSERT_TRUE(WallUpdateEnd::Converged == up.hello(0, STORED, false, true, STORED, 40000));
  TEST_ASSERT_EQUAL(1, up.attempts[0]);
}

static void test_an_image_that_crashes_after_its_hello_is_offered_three_times_only() {
  rows[0].rev = STORED;
  rows[0].rescue = true;
  uint32_t now = 1000;
  for (int i = 0; i < WALL_UPDATE_ATTEMPT_CAP; i++) {
    TEST_ASSERT_EQUAL(0, up.nextCandidate(rows, 1, STORED, now));
    up.offered(0, true, now);
    up.answer(0, state(wl_UpdatePhase_UPDATE_INSTALLED), STORED, now + 20000);
    // Back and looking well, then it dies again and returns in rescue mode.
    up.hello(0, STORED, false, true, STORED, now + 40000);
    now += 100000;
  }
  TEST_ASSERT_TRUE(up.blocked[0]);
  TEST_ASSERT_EQUAL(-1, up.nextCandidate(rows, 1, STORED, now));
}

static void test_a_row_back_still_in_rescue_mode_did_not_take_the_image() {
  up.offered(0, true, 1000);
  up.answer(0, state(wl_UpdatePhase_UPDATE_INSTALLED), STORED, 20000);
  TEST_ASSERT_TRUE(WallUpdateEnd::StillRescue == up.hello(0, STORED, true, true, STORED, 40000));
  TEST_ASSERT_EQUAL(1, up.attempts[0]);
}

// ---- a fresh look --------------------------------------------------------------

static void block(int row) {
  for (int i = 0; i < WALL_UPDATE_ATTEMPT_CAP; i++) failOnce(row, 100000UL * (i + 1));
  TEST_ASSERT_TRUE(up.blocked[row]);
}

static void test_a_new_stored_image_forgives_every_row_and_lifts_the_hold() {
  block(0);
  up.newImage();
  TEST_ASSERT_FALSE(up.blocked[0]);
  TEST_ASSERT_EQUAL(0, up.attempts[0]);
  TEST_ASSERT_EQUAL(0, candidate(300200));
}

static void test_a_row_reporting_another_rev_than_before_is_forgiven() {
  block(0);
  up.noteRev(0, "old0000", "old0000");
  TEST_ASSERT_TRUE(up.blocked[0]);
  up.noteRev(0, "", "old0000");  // nothing known before is no change
  TEST_ASSERT_TRUE(up.blocked[0]);
  up.noteRev(0, "old0000", "hand456");  // someone uploaded to the row directly
  TEST_ASSERT_FALSE(up.blocked[0]);
}

static void test_the_rev_change_of_its_own_offer_forgives_nothing() {
  failOnce(0, 1000);
  up.offered(0, false, 100000);
  up.noteRev(0, "old0000", "bad9999");
  TEST_ASSERT_EQUAL(1, up.attempts[0]);
}

static void test_ten_minutes_of_unbroken_health_forgive_a_row() {
  block(0);
  up.health(0, true, 1000000);
  up.health(0, true, 1000000 + WALL_UPDATE_HEALTHY_FORGIVE_MS - 1);
  TEST_ASSERT_TRUE(up.blocked[0]);
  up.health(0, true, 1000000 + WALL_UPDATE_HEALTHY_FORGIVE_MS);
  TEST_ASSERT_FALSE(up.blocked[0]);
}

static void test_a_row_that_keeps_coming_back_never_fills_the_health_window() {
  block(0);
  uint32_t now = 1000000;
  for (int i = 0; i < 5; i++) {
    up.health(0, true, now);
    now += WALL_UPDATE_HEALTHY_FORGIVE_MS - 1000;
    up.health(0, true, now);
    up.hello(0, "old0000", false, true, STORED, now);  // it restarted
  }
  TEST_ASSERT_TRUE(up.blocked[0]);
  // Rescue mode and being away break the window as well.
  up.health(0, true, now);
  up.health(0, false, now + 1000);
  up.health(0, true, now + 2000);
  up.health(0, true, now + WALL_UPDATE_HEALTHY_FORGIVE_MS);
  TEST_ASSERT_TRUE(up.blocked[0]);
}

static void test_a_changed_rows_table_forgets_everything() {
  block(0);
  up.offered(1, false, 500000);
  up.reset();
  TEST_ASSERT_TRUE(WallUpdatePhase::Idle == up.phase);
  TEST_ASSERT_FALSE(up.blocked[0]);
  TEST_ASSERT_EQUAL(0, candidate(500001));
}

static void test_times_survive_the_millisecond_counter_wrapping() {
  const uint32_t nearWrap = 0xFFFFFF00UL;
  failOnce(0, nearWrap);
  TEST_ASSERT_EQUAL(-1, candidate(nearWrap + 1000));
  TEST_ASSERT_EQUAL(0, candidate(nearWrap + 100 + WALL_UPDATE_HOLDOFF_MS));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_the_first_row_on_another_rev_is_offered_in_either_direction);
  RUN_TEST(test_nothing_is_offered_without_a_stored_image);
  RUN_TEST(test_a_row_that_cannot_take_it_now_or_never_said_its_rev_is_passed_over);
  RUN_TEST(test_a_row_in_rescue_mode_is_offered_the_rev_it_reports);
  RUN_TEST(test_one_row_at_a_time);
  RUN_TEST(test_a_row_back_on_the_stored_rev_has_converged_and_the_next_follows_at_once);
  RUN_TEST(test_a_row_that_restarts_before_its_installed_was_read_has_converged_too);
  RUN_TEST(test_a_redial_on_the_old_boot_id_is_not_the_row_coming_back);
  RUN_TEST(test_a_failed_download_costs_an_attempt_and_holds_every_offer_off);
  RUN_TEST(test_three_failed_offers_block_the_row_and_the_next_row_is_served);
  RUN_TEST(test_a_row_busy_with_its_units_is_asked_again_later_at_no_cost);
  RUN_TEST(test_any_other_refusal_costs_an_attempt);
  RUN_TEST(test_an_unanswered_offer_is_made_again_later_at_no_cost);
  RUN_TEST(test_a_download_that_never_reports_its_end_times_out_at_a_cost);
  RUN_TEST(test_a_row_that_never_comes_back_times_out_at_a_cost);
  RUN_TEST(test_a_row_back_on_another_rev_rolled_back_at_a_cost);
  RUN_TEST(test_an_offer_costs_one_attempt_however_it_goes_wrong);
  RUN_TEST(test_a_result_about_another_image_or_row_or_no_offer_is_history);
  RUN_TEST(test_a_rescue_offer_costs_its_attempt_when_made_and_coming_back_well_keeps_it);
  RUN_TEST(test_an_image_that_crashes_after_its_hello_is_offered_three_times_only);
  RUN_TEST(test_a_row_back_still_in_rescue_mode_did_not_take_the_image);
  RUN_TEST(test_a_new_stored_image_forgives_every_row_and_lifts_the_hold);
  RUN_TEST(test_a_row_reporting_another_rev_than_before_is_forgiven);
  RUN_TEST(test_the_rev_change_of_its_own_offer_forgives_nothing);
  RUN_TEST(test_ten_minutes_of_unbroken_health_forgive_a_row);
  RUN_TEST(test_a_row_that_keeps_coming_back_never_fills_the_health_window);
  RUN_TEST(test_a_changed_rows_table_forgets_everything);
  RUN_TEST(test_times_survive_the_millisecond_counter_wrapping);
  return UNITY_END();
}
