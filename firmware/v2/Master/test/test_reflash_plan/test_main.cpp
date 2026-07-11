// Host-side tests for ReflashPlan.h (#205) — the pure planning + progress
// core of the unit reflash job: who gets the enter-bootloader opcode, who
// gets flashed, the progress state machine displayTask publishes, and the
// job-level MaintResult grading.

#include <ArduinoFake.h>
#include <unity.h>

#include "../../ReflashPlan.h"

void setUp() {}
void tearDown() {}

// --- plan derivation ---------------------------------------------------------

static void test_needs_reboot_only_for_sketch_units_off_the_bundle() {
  UnitFacts u;
  u.state = 1; u.fwStatus = 1;  // sketch, outdated
  TEST_ASSERT_TRUE(reflashUnitNeedsReboot(u));
  u.fwStatus = 2;               // sketch, unknown rev — v1 #114: qualifies
  TEST_ASSERT_TRUE(reflashUnitNeedsReboot(u));
  u.fwStatus = 0;               // already on the bundled rev — skip
  TEST_ASSERT_FALSE(reflashUnitNeedsReboot(u));
  u.state = 0; u.fwStatus = 2;  // silent
  TEST_ASSERT_FALSE(reflashUnitNeedsReboot(u));
  u.state = 2;                  // already in bootloader — no reboot needed
  TEST_ASSERT_FALSE(reflashUnitNeedsReboot(u));
}

static void test_collect_reboot_targets_fills_addresses() {
  UnitFacts facts[UNITS_AMOUNT];
  facts[0].state = 1; facts[0].fwStatus = 0;  // on bundle — skipped
  facts[1].state = 1; facts[1].fwStatus = 1;  // outdated
  facts[3].state = 1; facts[3].fwStatus = 2;  // unknown
  facts[4].state = 2;                         // bootloader — not a reboot target
  uint8_t addrs[UNITS_AMOUNT];
  int n = reflashCollectRebootTargets(facts, UNITS_AMOUNT, 1, addrs);
  TEST_ASSERT_EQUAL(2, n);
  TEST_ASSERT_EQUAL_UINT8(2, addrs[0]);  // base 1 + index 1
  TEST_ASSERT_EQUAL_UINT8(4, addrs[1]);  // base 1 + index 3
}

static void test_collect_flash_targets_takes_bootloader_units_only() {
  UnitFacts facts[UNITS_AMOUNT];
  facts[0].state = 2;
  facts[1].state = 1; facts[1].fwStatus = 1;  // still in sketch — not flashable
  facts[5].state = 2;
  uint8_t addrs[UNITS_AMOUNT];
  int n = reflashCollectFlashTargets(facts, UNITS_AMOUNT, 1, addrs);
  TEST_ASSERT_EQUAL(2, n);
  TEST_ASSERT_EQUAL_UINT8(1, addrs[0]);
  TEST_ASSERT_EQUAL_UINT8(6, addrs[1]);
}

static void test_batch_constants_match_v1() {
  TEST_ASSERT_EQUAL(2, REFLASH_BATCH_SIZE);
  TEST_ASSERT_EQUAL(15000UL, REFLASH_BATCH_SETTLE_MS);
  TEST_ASSERT_EQUAL(500, TWIBOOT_STARTUP_MS);
}

// --- progress state machine ----------------------------------------------------

static void test_fresh_progress_is_idle_and_not_in_progress() {
  ReflashProgress p;
  TEST_ASSERT_EQUAL(ReflashState::Idle, p.state);
  TEST_ASSERT_FALSE(reflashInProgress(p));
}

static void test_begin_enters_and_counts() {
  ReflashProgress p;
  reflashProgressBegin(p, 12);
  TEST_ASSERT_EQUAL(ReflashState::Entering, p.state);
  TEST_ASSERT_EQUAL(12, p.total);
  TEST_ASSERT_EQUAL(0, p.done);
  TEST_ASSERT_EQUAL(0, p.failed);
  TEST_ASSERT_TRUE(reflashInProgress(p));
}

static void test_unit_start_and_results_accumulate() {
  ReflashProgress p;
  reflashProgressBegin(p, 3);
  reflashProgressUnitStart(p, 5);
  TEST_ASSERT_EQUAL(ReflashState::Flashing, p.state);
  TEST_ASSERT_EQUAL_UINT8(5, p.currentAddr);
  reflashProgressUnitResult(p, true);
  reflashProgressUnitStart(p, 6);
  reflashProgressUnitResult(p, false);
  TEST_ASSERT_EQUAL(1, p.done);
  TEST_ASSERT_EQUAL(1, p.failed);
  TEST_ASSERT_TRUE(reflashInProgress(p));
}

static void test_settling_is_still_in_progress() {
  ReflashProgress p;
  reflashProgressBegin(p, 4);
  reflashProgressSettling(p);
  TEST_ASSERT_EQUAL(ReflashState::Settling, p.state);
  TEST_ASSERT_TRUE(reflashInProgress(p));
}

static void test_finish_grades_done_cancelled_failed() {
  ReflashProgress p;
  reflashProgressBegin(p, 2);
  reflashProgressUnitStart(p, 1);
  reflashProgressUnitResult(p, true);
  reflashProgressFinish(p, false);
  TEST_ASSERT_EQUAL(ReflashState::Done, p.state);
  TEST_ASSERT_EQUAL_UINT8(0, p.currentAddr);
  TEST_ASSERT_FALSE(reflashInProgress(p));

  ReflashProgress c;
  reflashProgressBegin(c, 2);
  reflashProgressFinish(c, true);
  TEST_ASSERT_EQUAL(ReflashState::Cancelled, c.state);

  ReflashProgress f;
  reflashProgressBegin(f, 2);
  reflashProgressUnitStart(f, 1);
  reflashProgressUnitResult(f, false);
  reflashProgressFinish(f, false);
  TEST_ASSERT_EQUAL(ReflashState::Failed, f.state);
}

static void test_cancel_wins_over_failures_in_grading() {
  ReflashProgress p;
  reflashProgressBegin(p, 3);
  reflashProgressUnitStart(p, 1);
  reflashProgressUnitResult(p, false);
  reflashProgressFinish(p, true);
  TEST_ASSERT_EQUAL(ReflashState::Cancelled, p.state);
}

static void test_state_names() {
  TEST_ASSERT_EQUAL_STRING("idle", reflashStateName(ReflashState::Idle));
  TEST_ASSERT_EQUAL_STRING("entering", reflashStateName(ReflashState::Entering));
  TEST_ASSERT_EQUAL_STRING("flashing", reflashStateName(ReflashState::Flashing));
  TEST_ASSERT_EQUAL_STRING("settling", reflashStateName(ReflashState::Settling));
  TEST_ASSERT_EQUAL_STRING("done", reflashStateName(ReflashState::Done));
  TEST_ASSERT_EQUAL_STRING("cancelled", reflashStateName(ReflashState::Cancelled));
  TEST_ASSERT_EQUAL_STRING("failed", reflashStateName(ReflashState::Failed));
}

// --- job-level MaintResult grading ---------------------------------------------

static void test_classify_done_job_is_ok() {
  ReflashProgress p;
  reflashProgressBegin(p, 2);
  reflashProgressUnitStart(p, 1);
  reflashProgressUnitResult(p, true);
  reflashProgressUnitStart(p, 2);
  reflashProgressUnitResult(p, true);
  reflashProgressFinish(p, false);
  MaintReason reason;
  TEST_ASSERT_EQUAL(MaintOutcome::Ok, classifyReflashOutcome(p, reason));
  TEST_ASSERT_EQUAL(MaintReason::None, reason);
}

static void test_classify_failed_and_cancelled_jobs() {
  ReflashProgress f;
  reflashProgressBegin(f, 1);
  reflashProgressUnitStart(f, 1);
  reflashProgressUnitResult(f, false);
  reflashProgressFinish(f, false);
  MaintReason reason;
  TEST_ASSERT_EQUAL(MaintOutcome::PostconditionFail,
                    classifyReflashOutcome(f, reason));

  ReflashProgress c;
  reflashProgressBegin(c, 1);
  reflashProgressFinish(c, true);
  TEST_ASSERT_EQUAL(MaintOutcome::PostconditionFail,
                    classifyReflashOutcome(c, reason));
}

static void test_empty_plan_finishes_done_and_ok() {
  // Every unit already on the bundled rev: begin(0) + finish is a no-op job
  // that must still grade ok — the v1 semantics for "nothing to do".
  ReflashProgress p;
  reflashProgressBegin(p, 0);
  reflashProgressFinish(p, false);
  TEST_ASSERT_EQUAL(ReflashState::Done, p.state);
  MaintReason reason;
  TEST_ASSERT_EQUAL(MaintOutcome::Ok, classifyReflashOutcome(p, reason));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_needs_reboot_only_for_sketch_units_off_the_bundle);
  RUN_TEST(test_collect_reboot_targets_fills_addresses);
  RUN_TEST(test_collect_flash_targets_takes_bootloader_units_only);
  RUN_TEST(test_batch_constants_match_v1);
  RUN_TEST(test_fresh_progress_is_idle_and_not_in_progress);
  RUN_TEST(test_begin_enters_and_counts);
  RUN_TEST(test_unit_start_and_results_accumulate);
  RUN_TEST(test_settling_is_still_in_progress);
  RUN_TEST(test_finish_grades_done_cancelled_failed);
  RUN_TEST(test_cancel_wins_over_failures_in_grading);
  RUN_TEST(test_state_names);
  RUN_TEST(test_classify_done_job_is_ok);
  RUN_TEST(test_classify_failed_and_cancelled_jobs);
  RUN_TEST(test_empty_plan_finishes_done_and_ok);
  return UNITY_END();
}
