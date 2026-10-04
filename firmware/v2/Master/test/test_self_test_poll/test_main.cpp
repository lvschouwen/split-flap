// Host-side tests for shared/SelfTestPoll.h — what a row master makes of a
// unit's GET_SELF_TEST replies while a test runs. Both row masters feed it.

#include <unity.h>

#include "SelfTestPoll.h"

void setUp() {}
void tearDown() {}

namespace {

UnitSelfTestReading reading(uint8_t state, uint16_t steps = 0,
                            uint16_t window = 0, uint16_t ms = 0,
                            uint8_t reason = SELFTEST_REASON_NONE) {
  UnitSelfTestReading r;
  r.state = state;
  r.stepsPerRev = steps;
  r.hallWindowSteps = window;
  r.revTimeMs = ms;
  r.reason = reason;
  return r;
}

const UnitSelfTestReading kNone{};

}  // namespace

static void test_running_then_ok_carries_measurements() {
  SelfTestPoll p;
  SelfTestSlot slot;
  selfTestPollBegin(p, 1000);
  TEST_ASSERT_EQUAL(
      (int)SelfTestOutcome::Pending,
      (int)selfTestPollObserve(p, true, reading(SELFTEST_STATE_RUNNING), 1500,
                               slot));
  TEST_ASSERT_EQUAL(
      (int)SelfTestOutcome::Ok,
      (int)selfTestPollObserve(p, true,
                               reading(SELFTEST_STATE_OK, 2040, 60, 4100),
                               15000, slot));
  TEST_ASSERT_EQUAL_UINT16(2040, slot.stepsPerRev);
  TEST_ASSERT_EQUAL_UINT16(60, slot.hallWindowSteps);
  TEST_ASSERT_EQUAL_UINT16(4100, slot.revTimeMs);
}

// The unit still answers with the previous test's result while this one
// queues behind a move: the first poll must not be taken for the outcome.
static void test_stale_terminal_from_a_previous_test_is_not_accepted() {
  SelfTestPoll p;
  SelfTestSlot slot;
  selfTestPollBegin(p, 0);
  UnitSelfTestReading old = reading(SELFTEST_STATE_OK, 2038, 59, 4000);
  for (uint32_t t = 500; t <= 3000; t += 500) {
    TEST_ASSERT_EQUAL((int)SelfTestOutcome::Pending,
                      (int)selfTestPollObserve(p, true, old, t, slot));
  }
  TEST_ASSERT_EQUAL_UINT16(0, slot.stepsPerRev);
  // The new test runs and fails: that one is this test's result.
  selfTestPollObserve(p, true, reading(SELFTEST_STATE_RUNNING), 3500, slot);
  TEST_ASSERT_EQUAL(
      (int)SelfTestOutcome::UnitFailed,
      (int)selfTestPollObserve(
          p, true,
          reading(SELFTEST_STATE_FAILED, 0, 12, 0, SELFTEST_REASON_HALL_NEVER),
          9000, slot));
  TEST_ASSERT_EQUAL_UINT8(SELFTEST_REASON_HALL_NEVER, slot.unitReason);
  TEST_ASSERT_EQUAL_UINT16(12, slot.hallWindowSteps);
}

// Every poll of the RUNNING window lost to the bus: a terminal that differs
// from the first reading is still provably this test's.
static void test_terminal_differing_from_baseline_is_fresh_without_running() {
  SelfTestPoll p;
  SelfTestSlot slot;
  selfTestPollBegin(p, 0);
  selfTestPollObserve(p, true, reading(SELFTEST_STATE_OK, 2038, 59, 4000), 500,
                      slot);
  TEST_ASSERT_EQUAL(
      (int)SelfTestOutcome::Ok,
      (int)selfTestPollObserve(p, true,
                               reading(SELFTEST_STATE_OK, 2039, 58, 4010),
                               14000, slot));
  TEST_ASSERT_EQUAL_UINT16(2039, slot.stepsPerRev);
}

static void test_unreadable_replies_mean_unsupported_firmware() {
  SelfTestPoll p;
  SelfTestSlot slot;
  selfTestPollBegin(p, 0);
  TEST_ASSERT_EQUAL((int)SelfTestOutcome::Pending,
                    (int)selfTestPollObserve(p, false, kNone, 500, slot));
  TEST_ASSERT_EQUAL((int)SelfTestOutcome::Pending,
                    (int)selfTestPollObserve(p, false, kNone, 1000, slot));
  TEST_ASSERT_EQUAL((int)SelfTestOutcome::Unsupported,
                    (int)selfTestPollObserve(p, false, kNone, 1500, slot));
}

// Bus glitches after a valid reading are not "unsupported".
static void test_bad_polls_after_a_valid_one_do_not_mean_unsupported() {
  SelfTestPoll p;
  SelfTestSlot slot;
  selfTestPollBegin(p, 0);
  selfTestPollObserve(p, true, reading(SELFTEST_STATE_RUNNING), 500, slot);
  for (uint32_t t = 1000; t <= 5000; t += 500) {
    TEST_ASSERT_EQUAL((int)SelfTestOutcome::Pending,
                      (int)selfTestPollObserve(p, false, kNone, t, slot));
  }
}

static void test_never_started_times_out() {
  SelfTestPoll p;
  SelfTestSlot slot;
  selfTestPollBegin(p, 100);
  TEST_ASSERT_EQUAL(
      (int)SelfTestOutcome::Pending,
      (int)selfTestPollObserve(p, true, reading(SELFTEST_STATE_NEVER),
                               100 + SELF_TEST_TIMEOUT_MS - 1, slot));
  TEST_ASSERT_EQUAL(
      (int)SelfTestOutcome::Timeout,
      (int)selfTestPollObserve(p, true, reading(SELFTEST_STATE_NEVER),
                               100 + SELF_TEST_TIMEOUT_MS, slot));
}

// Time spent queued behind a move must not eat the test's own budget.
static void test_window_rearms_when_running_is_first_seen() {
  SelfTestPoll p;
  SelfTestSlot slot;
  selfTestPollBegin(p, 0);
  uint32_t late = SELF_TEST_TIMEOUT_MS - 500;
  selfTestPollObserve(p, true, reading(SELFTEST_STATE_RUNNING), late, slot);
  TEST_ASSERT_EQUAL(
      (int)SelfTestOutcome::Pending,
      (int)selfTestPollObserve(p, true, reading(SELFTEST_STATE_RUNNING),
                               late + SELF_TEST_TIMEOUT_MS - 1, slot));
  TEST_ASSERT_EQUAL(
      (int)SelfTestOutcome::Timeout,
      (int)selfTestPollObserve(p, true, reading(SELFTEST_STATE_RUNNING),
                               late + SELF_TEST_TIMEOUT_MS, slot));
}

static void test_millis_wrap_does_not_time_out_early() {
  SelfTestPoll p;
  SelfTestSlot slot;
  selfTestPollBegin(p, 0xFFFFFF00u);
  TEST_ASSERT_EQUAL(
      (int)SelfTestOutcome::Pending,
      (int)selfTestPollObserve(p, true, reading(SELFTEST_STATE_NEVER), 0x100,
                               slot));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_running_then_ok_carries_measurements);
  RUN_TEST(test_stale_terminal_from_a_previous_test_is_not_accepted);
  RUN_TEST(test_terminal_differing_from_baseline_is_fresh_without_running);
  RUN_TEST(test_unreadable_replies_mean_unsupported_firmware);
  RUN_TEST(test_bad_polls_after_a_valid_one_do_not_mean_unsupported);
  RUN_TEST(test_never_started_times_out);
  RUN_TEST(test_window_rearms_when_running_is_first_seen);
  RUN_TEST(test_millis_wrap_does_not_time_out_early);
  return UNITY_END();
}
