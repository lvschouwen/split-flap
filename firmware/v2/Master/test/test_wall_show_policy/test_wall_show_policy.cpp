// Native tests for WallShowPolicy.h: when the rows of a wall flip, and what
// the master does with its own row.
#include <unity.h>

#include "../../WallShowPolicy.h"
#include "../../ClusterLayout.h"  // CLUSTER_MAX_MEMBERS

void setUp() {}
void tearDown() {}

// ---- the flip instant --------------------------------------------------------------

static void test_typed_text_flips_400_ms_from_now() {
  TEST_ASSERT_TRUE(1000000400ULL == wallCommitAtMs(1000000000ULL, true));
}

static void test_without_a_synced_clock_rows_flip_on_arrival() {
  TEST_ASSERT_TRUE(0 == wallCommitAtMs(1000000000ULL, false));
}

// ---- the clock: the next minute is announced ahead ---------------------------------

static const uint64_t MINUTE = 1791278040000ULL;  // a minute boundary, in ms

static void test_inside_a_minute_the_clock_shows_that_minute_soon() {
  WallClockTarget t = wallClockTarget(MINUTE + 12345);
  TEST_ASSERT_TRUE(MINUTE / 1000 == (uint64_t)t.minuteEpochS);
  TEST_ASSERT_TRUE(MINUTE + 12345 + 400 == t.commitAtMs);
}

// From 3 s before a minute the next one is what the wall is given, to flip at
// the boundary itself: the 1 Hz ticker lands once in the second from -3 to
// -2, so every row gets it at least 2 s ahead.
static void test_the_next_minute_is_given_ahead_to_flip_at_the_boundary() {
  WallClockTarget before = wallClockTarget(MINUTE + 56999);
  TEST_ASSERT_TRUE(MINUTE / 1000 == (uint64_t)before.minuteEpochS);
  WallClockTarget ahead = wallClockTarget(MINUTE + 57000);
  TEST_ASSERT_TRUE(MINUTE / 1000 + 60 == (uint64_t)ahead.minuteEpochS);
  TEST_ASSERT_TRUE(MINUTE + 60000 == ahead.commitAtMs);
  WallClockTarget last = wallClockTarget(MINUTE + 59999);
  TEST_ASSERT_TRUE(MINUTE / 1000 + 60 == (uint64_t)last.minuteEpochS);
  TEST_ASSERT_TRUE(MINUTE + 60000 == last.commitAtMs);
  // At the boundary the target is the same minute: nothing new to send.
  WallClockTarget at = wallClockTarget(MINUTE + 60000);
  TEST_ASSERT_TRUE(MINUTE / 1000 + 60 == (uint64_t)at.minuteEpochS);
}

static void test_the_lead_is_within_what_a_row_will_wait() {
  TEST_ASSERT_TRUE(WALL_CLOCK_AHEAD_MS >= 2000 + 1000);  // 2 s, and the ticker's one second
  TEST_ASSERT_TRUE(WALL_CLOCK_AHEAD_MS <= CLUSTER_COMMIT_MAX_DELAY_MS);
}

// Each board flips a step after the one before it; the last one of a full
// table must still be inside what a row will wait for a clock minute.
static void test_boards_flip_a_step_apart() {
  TEST_ASSERT_TRUE(wallRowFlipAtMs(MINUTE, 0) == MINUTE);
  TEST_ASSERT_TRUE(wallRowFlipAtMs(MINUTE, 1) == MINUTE + WALL_ROW_STAGGER_MS);
  TEST_ASSERT_TRUE(wallRowFlipAtMs(MINUTE, 3) == MINUTE + 3 * WALL_ROW_STAGGER_MS);
  TEST_ASSERT_TRUE(WALL_CLOCK_AHEAD_MS + (CLUSTER_MAX_MEMBERS - 1) * WALL_ROW_STAGGER_MS <=
                   CLUSTER_COMMIT_MAX_DELAY_MS);
}

static void test_a_flip_on_arrival_stays_on_arrival() {
  TEST_ASSERT_TRUE(wallRowFlipAtMs(0, 0) == 0);
  TEST_ASSERT_TRUE(wallRowFlipAtMs(0, 5) == 0);
}

// ---- the master's own row ----------------------------------------------------------

static WallOwnRow shown(const char* text) {
  WallOwnRow o;
  o.text = text;
  o.displayText = text;
  return o;
}

static void test_a_new_text_waits_for_its_instant_then_is_queued() {
  WallOwnRow o = shown("OLD");
  o.text = "NEW";
  o.pending = true;
  o.msUntilDue = 250;
  TEST_ASSERT_TRUE(WallOwnAction::Wait == wallOwnRowAction(o));
  o.msUntilDue = 0;
  TEST_ASSERT_TRUE(WallOwnAction::Show == wallOwnRowAction(o));
}

static void test_a_unit_update_holds_the_new_text_back() {
  WallOwnRow o = shown("OLD");
  o.text = "NEW";
  o.pending = true;
  o.reflashing = true;
  TEST_ASSERT_TRUE(WallOwnAction::Wait == wallOwnRowAction(o));
}

static void test_a_row_showing_its_text_is_left_alone() {
  TEST_ASSERT_TRUE(WallOwnAction::None == wallOwnRowAction(shown("SAME")));
}

// After a notification, a homing or a stop the row is not showing the wall's
// text any more: it is shown again.
static void test_a_row_showing_something_else_is_given_its_text_again() {
  WallOwnRow o = shown("WALL TEXT");
  o.displayText = "DING";
  TEST_ASSERT_TRUE(WallOwnAction::Show == wallOwnRowAction(o));
}

static void test_the_text_is_not_shown_again_over_a_notification_or_while_quiet_or_busy() {
  WallOwnRow o = shown("WALL TEXT");
  o.displayText = "DING";
  o.notification = true;
  TEST_ASSERT_TRUE(WallOwnAction::None == wallOwnRowAction(o));
  o.notification = false;
  o.quiet = true;
  TEST_ASSERT_TRUE(WallOwnAction::None == wallOwnRowAction(o));
  o.quiet = false;
  o.displayBusy = true;
  TEST_ASSERT_TRUE(WallOwnAction::None == wallOwnRowAction(o));
  o.displayBusy = false;
  o.reflashing = true;
  TEST_ASSERT_TRUE(WallOwnAction::None == wallOwnRowAction(o));
}

// A blank row (after Stop) stays blank until the wall is given something.
static void test_an_empty_text_is_never_shown_again_by_itself() {
  WallOwnRow o = shown("");
  o.displayText = "LEFTOVER";
  TEST_ASSERT_TRUE(WallOwnAction::None == wallOwnRowAction(o));
}

// A new text is new even while quiet holds the wall: quiet is the producers'
// rule (they stand down), not this one's.
static void test_a_pending_text_is_queued_even_over_a_notification() {
  WallOwnRow o = shown("OLD");
  o.text = "NEW";
  o.pending = true;
  o.notification = true;
  TEST_ASSERT_TRUE(WallOwnAction::Show == wallOwnRowAction(o));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_typed_text_flips_400_ms_from_now);
  RUN_TEST(test_without_a_synced_clock_rows_flip_on_arrival);
  RUN_TEST(test_inside_a_minute_the_clock_shows_that_minute_soon);
  RUN_TEST(test_the_next_minute_is_given_ahead_to_flip_at_the_boundary);
  RUN_TEST(test_the_lead_is_within_what_a_row_will_wait);
  RUN_TEST(test_boards_flip_a_step_apart);
  RUN_TEST(test_a_flip_on_arrival_stays_on_arrival);
  RUN_TEST(test_a_new_text_waits_for_its_instant_then_is_queued);
  RUN_TEST(test_a_unit_update_holds_the_new_text_back);
  RUN_TEST(test_a_row_showing_its_text_is_left_alone);
  RUN_TEST(test_a_row_showing_something_else_is_given_its_text_again);
  RUN_TEST(test_the_text_is_not_shown_again_over_a_notification_or_while_quiet_or_busy);
  RUN_TEST(test_an_empty_text_is_never_shown_again_by_itself);
  RUN_TEST(test_a_pending_text_is_queued_even_over_a_notification);
  return UNITY_END();
}
