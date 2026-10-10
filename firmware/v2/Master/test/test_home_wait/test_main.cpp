// Host-side tests for shared/HomeWait.h — how a row master waits for a unit's
// home search and grades the job by what the unit then reports. Both row
// masters feed it.

#include <unity.h>
#include <string.h>

#include "HomeWait.h"

void setUp() {}
void tearDown() {}

namespace {

const uint8_t kHomedIdle = UNIT_FLAG_HOMED;
const uint8_t kHomedMoving = UNIT_FLAG_HOMED | UNIT_FLAG_MOVING;
const uint8_t kSearching = UNIT_FLAG_MOVING;
const uint8_t kFailedIdle = UNIT_FLAG_LAST_HOME_FAILED | UNIT_FLAG_HALL_NEVER;

int observe(HomeWait& w, bool ok, uint8_t flags, uint32_t now) {
  return (int)homeWaitObserve(w, ok, flags, now);
}

}  // namespace

static void test_a_search_that_finds_home_ends_found() {
  HomeWait w;
  homeWaitBegin(w, 1000);
  TEST_ASSERT_EQUAL((int)HomeWaitOutcome::Pending, observe(w, true, kHomedMoving, 1250));
  TEST_ASSERT_EQUAL((int)HomeWaitOutcome::Pending, observe(w, true, kHomedMoving, 4000));
  TEST_ASSERT_EQUAL((int)HomeWaitOutcome::Pending, observe(w, true, kHomedIdle, 4250));
  TEST_ASSERT_EQUAL((int)HomeWaitOutcome::Found,
                    observe(w, true, kHomedIdle, 4250 + HOME_WAIT_IDLE_MS));
}

static void test_a_search_that_fails_ends_not_found() {
  HomeWait w;
  homeWaitBegin(w, 0);
  TEST_ASSERT_EQUAL((int)HomeWaitOutcome::Pending, observe(w, true, kSearching, 250));
  TEST_ASSERT_EQUAL((int)HomeWaitOutcome::Pending, observe(w, true, kFailedIdle, 20000));
  TEST_ASSERT_EQUAL((int)HomeWaitOutcome::NotFound,
                    observe(w, true, kFailedIdle, 20000 + HOME_WAIT_IDLE_MS));
}

// The unit reads idle for a moment between the move in flight and the search
// queued behind it: the homed flag of before the search must not end the job.
static void test_one_idle_reading_does_not_end_the_wait() {
  HomeWait w;
  homeWaitBegin(w, 0);
  TEST_ASSERT_EQUAL((int)HomeWaitOutcome::Pending, observe(w, true, kHomedMoving, 250));
  TEST_ASSERT_EQUAL((int)HomeWaitOutcome::Pending, observe(w, true, kHomedIdle, 500));
  TEST_ASSERT_EQUAL((int)HomeWaitOutcome::Pending, observe(w, true, kSearching, 750));
  TEST_ASSERT_EQUAL((int)HomeWaitOutcome::Pending, observe(w, true, kFailedIdle, 21000));
  TEST_ASSERT_EQUAL((int)HomeWaitOutcome::NotFound,
                    observe(w, true, kFailedIdle, 21000 + HOME_WAIT_IDLE_MS));
}

// A unit refuses the command for 30 s after a failed search and keeps saying
// so: the job fails with the unit's state.
static void test_a_refused_command_reads_as_the_failure_it_stands_on() {
  HomeWait w;
  homeWaitBegin(w, 0);
  TEST_ASSERT_EQUAL((int)HomeWaitOutcome::Pending, observe(w, true, kFailedIdle, 250));
  TEST_ASSERT_EQUAL((int)HomeWaitOutcome::NotFound,
                    observe(w, true, kFailedIdle, 250 + HOME_WAIT_IDLE_MS));
}

static void test_an_unhomed_unit_standing_still_is_not_found() {
  HomeWait w;
  homeWaitBegin(w, 0);
  observe(w, true, 0, 250);
  TEST_ASSERT_EQUAL((int)HomeWaitOutcome::NotFound,
                    observe(w, true, 0, 250 + HOME_WAIT_IDLE_MS));
}

static void test_an_unread_poll_restarts_the_idle_time() {
  HomeWait w;
  homeWaitBegin(w, 0);
  observe(w, true, kHomedIdle, 250);
  TEST_ASSERT_EQUAL((int)HomeWaitOutcome::Pending, observe(w, false, 0, 500));
  TEST_ASSERT_EQUAL((int)HomeWaitOutcome::Pending, observe(w, true, kHomedIdle, 800));
  TEST_ASSERT_EQUAL((int)HomeWaitOutcome::Found,
                    observe(w, true, kHomedIdle, 800 + HOME_WAIT_IDLE_MS));
}

static void test_still_turning_at_the_end_of_the_wait_is_not_ended() {
  HomeWait w;
  homeWaitBegin(w, 0);
  observe(w, true, kSearching, 250);
  TEST_ASSERT_EQUAL((int)HomeWaitOutcome::Pending,
                    observe(w, true, kSearching, HOME_WAIT_TIMEOUT_MS - 1));
  TEST_ASSERT_EQUAL((int)HomeWaitOutcome::NotEnded,
                    observe(w, true, kSearching, HOME_WAIT_TIMEOUT_MS));
}

static void test_a_unit_never_read_is_no_answer() {
  HomeWait w;
  homeWaitBegin(w, 0);
  TEST_ASSERT_EQUAL((int)HomeWaitOutcome::Pending, observe(w, false, 0, 250));
  TEST_ASSERT_EQUAL((int)HomeWaitOutcome::NoAnswer,
                    observe(w, false, 0, HOME_WAIT_TIMEOUT_MS));
}

static void test_millis_wrap_does_not_end_the_wait_early() {
  HomeWait w;
  homeWaitBegin(w, 0xFFFFFF00u);
  TEST_ASSERT_EQUAL((int)HomeWaitOutcome::Pending, observe(w, true, kSearching, 0x100));
  TEST_ASSERT_EQUAL((int)HomeWaitOutcome::Pending, observe(w, true, kHomedIdle, 0x200));
  TEST_ASSERT_EQUAL((int)HomeWaitOutcome::Found,
                    observe(w, true, kHomedIdle, 0x200 + HOME_WAIT_IDLE_MS));
}

static void test_grades() {
  MaintGrade g = maintGradeHome(HomeWaitOutcome::Found);
  TEST_ASSERT_TRUE(g.outcome == MaintOutcome::Ok && g.reason == MaintReason::None);
  g = maintGradeHome(HomeWaitOutcome::NotFound);
  TEST_ASSERT_TRUE(g.outcome == MaintOutcome::PostconditionFail &&
                   g.reason == MaintReason::HomeNotFound);
  g = maintGradeHome(HomeWaitOutcome::NotEnded);
  TEST_ASSERT_TRUE(g.outcome == MaintOutcome::PostconditionFail &&
                   g.reason == MaintReason::HomeNotEnded);
  g = maintGradeHome(HomeWaitOutcome::NoAnswer);
  TEST_ASSERT_TRUE(g.outcome == MaintOutcome::WireFail && g.reason == MaintReason::None);
  TEST_ASSERT_EQUAL_STRING("home-not-found", maintReasonName(MaintReason::HomeNotFound));
  TEST_ASSERT_EQUAL_STRING("home-not-ended", maintReasonName(MaintReason::HomeNotEnded));
}

static void test_home_all_fails_on_any_unit_that_answered_not_found() {
  HomeAllTally t;
  homeAllTallyAdd(t, true, kHomedIdle);
  homeAllTallyAdd(t, false, 0);  // a silent unit is the verdicts' to report
  TEST_ASSERT_TRUE(maintGradeHomeAll(t).outcome == MaintOutcome::Ok);
  homeAllTallyAdd(t, true, kFailedIdle);
  MaintGrade g = maintGradeHomeAll(t);
  TEST_ASSERT_TRUE(g.outcome == MaintOutcome::PostconditionFail &&
                   g.reason == MaintReason::HomeNotFound);
  TEST_ASSERT_EQUAL(1, t.notFound);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_a_search_that_finds_home_ends_found);
  RUN_TEST(test_a_search_that_fails_ends_not_found);
  RUN_TEST(test_one_idle_reading_does_not_end_the_wait);
  RUN_TEST(test_a_refused_command_reads_as_the_failure_it_stands_on);
  RUN_TEST(test_an_unhomed_unit_standing_still_is_not_found);
  RUN_TEST(test_an_unread_poll_restarts_the_idle_time);
  RUN_TEST(test_still_turning_at_the_end_of_the_wait_is_not_ended);
  RUN_TEST(test_a_unit_never_read_is_no_answer);
  RUN_TEST(test_millis_wrap_does_not_end_the_wait_early);
  RUN_TEST(test_grades);
  RUN_TEST(test_home_all_fails_on_any_unit_that_answered_not_found);
  return UNITY_END();
}
