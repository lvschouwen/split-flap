#include <unity.h>
#include <stdint.h>
#include "BootUpdatePlan.h"

void setUp() {}
void tearDown() {}

static BootUpdateReport makeReport(uint8_t state, uint8_t lock) {
  BootUpdateReport r;
  r.state = state;
  r.lockByte = lock;
  return r;
}

static void test_new_exits_immediately() {
  BootUpdatePlan p = bootUpdateDecide(makeReport(BOOT_STATE_NEW, 0xFF));
  TEST_ASSERT_FALSE(p.needStage1);
  TEST_ASSERT_FALSE(p.needStage2);
  TEST_ASSERT_EQUAL(BOOT_PLAN_ALREADY_NEW, p.terminal);
}

static void test_new_exits_before_lock_check() {
  // A NEW unit whose lock bits forbid boot writes still reports ALREADY_NEW,
  // not LOCK_REFUSED (reviewer finding 6: check order).
  BootUpdatePlan p = bootUpdateDecide(makeReport(BOOT_STATE_NEW, 0x00));
  TEST_ASSERT_EQUAL(BOOT_PLAN_ALREADY_NEW, p.terminal);
}

static void test_old_with_open_lock_needs_both_stages() {
  BootUpdatePlan p = bootUpdateDecide(makeReport(BOOT_STATE_OLD, 0xFF));
  TEST_ASSERT_TRUE(p.needStage1);
  TEST_ASSERT_TRUE(p.needStage2);
  TEST_ASSERT_EQUAL(BOOT_PLAN_PROCEED, p.terminal);
}

static void test_old_with_locked_boot_section_refuses() {
  // BLB1 = 0 forbids SPM writes to boot section pages.
  BootUpdatePlan p = bootUpdateDecide(makeReport(BOOT_STATE_OLD, 0x00));
  TEST_ASSERT_FALSE(p.needStage1);
  TEST_ASSERT_FALSE(p.needStage2);
  TEST_ASSERT_EQUAL(BOOT_PLAN_LOCK_REFUSED, p.terminal);
}

static void test_page7_installed_needs_stage2_only() {
  BootUpdatePlan p = bootUpdateDecide(
      makeReport(BOOT_STATE_PAGE7_INSTALLED, 0xFF));
  TEST_ASSERT_FALSE(p.needStage1);
  TEST_ASSERT_TRUE(p.needStage2);
  TEST_ASSERT_EQUAL(BOOT_PLAN_PROCEED, p.terminal);
}

static void test_trampoline_needs_stage2_only() {
  BootUpdatePlan p = bootUpdateDecide(
      makeReport(BOOT_STATE_TRAMPOLINE, 0xFF));
  TEST_ASSERT_FALSE(p.needStage1);
  TEST_ASSERT_TRUE(p.needStage2);
  TEST_ASSERT_EQUAL(BOOT_PLAN_PROCEED, p.terminal);
}

static void test_unknown_state_refuses() {
  BootUpdatePlan p = bootUpdateDecide(makeReport(BOOT_STATE_UNKNOWN, 0xFF));
  TEST_ASSERT_FALSE(p.needStage1);
  TEST_ASSERT_FALSE(p.needStage2);
  TEST_ASSERT_EQUAL(BOOT_PLAN_UNKNOWN_STATE, p.terminal);
}

static void test_page7_installed_with_locked_boot_refuses() {
  // Both stages use SPM to write boot-section pages; locked = refuse.
  BootUpdatePlan p = bootUpdateDecide(
      makeReport(BOOT_STATE_PAGE7_INSTALLED, 0x00));
  TEST_ASSERT_FALSE(p.needStage2);
  TEST_ASSERT_EQUAL(BOOT_PLAN_LOCK_REFUSED, p.terminal);
}

// #518: a lock nobody can read does not block the update; a lock that reads
// closed still does.
static void test_unreadable_lock_proceeds_in_every_updatable_state() {
  const uint8_t states[] = {BOOT_STATE_OLD, BOOT_STATE_PAGE7_INSTALLED,
                            BOOT_STATE_TRAMPOLINE};
  for (uint8_t st : states) {
    BootUpdateReport r = makeReport(st, 0x00);  // placeholder that READS closed
    r.lockFuseReadable = false;
    BootUpdatePlan p = bootUpdateDecide(r);
    TEST_ASSERT_EQUAL(BOOT_PLAN_PROCEED, p.terminal);
    TEST_ASSERT_TRUE(p.needStage2);
    TEST_ASSERT_EQUAL(st == BOOT_STATE_OLD, p.needStage1);
  }
}

static void test_unreadable_lock_does_not_rescue_an_unknown_state() {
  BootUpdateReport r = makeReport(BOOT_STATE_UNKNOWN, 0xFF);
  r.lockFuseReadable = false;
  TEST_ASSERT_EQUAL(BOOT_PLAN_UNKNOWN_STATE, bootUpdateDecide(r).terminal);
}

// --- reading the unit while a stage runs (#516) ---

static BootUpdateReport during(uint8_t state, uint8_t lastResult) {
  BootUpdateReport r = makeReport(state, 0xFF);
  r.lastResult = lastResult;
  return r;
}

static void test_result_failure_mapping() {
  TEST_ASSERT_EQUAL(BOOT_FAIL_BUSY, bootResultFailure(BOOT_RESULT_REFUSED_BUSY));
  TEST_ASSERT_EQUAL(BOOT_FAIL_LOCK, bootResultFailure(BOOT_RESULT_REFUSED_LOCK));
  TEST_ASSERT_EQUAL(BOOT_FAIL_STATE, bootResultFailure(BOOT_RESULT_REFUSED_STATE));
  TEST_ASSERT_EQUAL(BOOT_FAIL_VERIFY, bootResultFailure(BOOT_RESULT_VERIFY_FAILED));
  // Neither "nothing yet" nor a success is a failure.
  TEST_ASSERT_EQUAL(BOOT_FAIL_NONE, bootResultFailure(BOOT_RESULT_NONE));
  TEST_ASSERT_EQUAL(BOOT_FAIL_NONE, bootResultFailure(BOOT_RESULT_STAGE1_OK));
  TEST_ASSERT_EQUAL(BOOT_FAIL_NONE, bootResultFailure(BOOT_RESULT_STAGE2_OK));
  TEST_ASSERT_EQUAL(BOOT_FAIL_NONE, bootResultFailure(200));
}

static void test_stage2_poll_state_new_is_done_whatever_the_result() {
  for (int r = 0; r < BOOT_RESULT_COUNT; r++) {
    TEST_ASSERT_EQUAL(BOOT_POLL_DONE,
                      bootStage2Poll(during(BOOT_STATE_NEW, (uint8_t)r),
                                     BOOT_RESULT_NONE));
  }
}

static void test_stage2_poll_fresh_failure_ends_the_poll() {
  const uint8_t failures[] = {BOOT_RESULT_REFUSED_BUSY, BOOT_RESULT_REFUSED_LOCK,
                              BOOT_RESULT_REFUSED_STATE, BOOT_RESULT_VERIFY_FAILED};
  for (uint8_t f : failures) {
    TEST_ASSERT_EQUAL(BOOT_POLL_FAILED,
                      bootStage2Poll(during(BOOT_STATE_PAGE7_INSTALLED, f),
                                     BOOT_RESULT_NONE));
  }
}

// The race the review found: a refusal left over from an earlier attempt is
// still in the unit's report when the first poll lands, before the unit has
// run the stage it was just asked for.
static void test_stage2_poll_ignores_a_result_that_was_there_before_the_send() {
  TEST_ASSERT_EQUAL(BOOT_POLL_WAIT,
                    bootStage2Poll(during(BOOT_STATE_PAGE7_INSTALLED,
                                          BOOT_RESULT_REFUSED_BUSY),
                                   BOOT_RESULT_REFUSED_BUSY));
  // ...and a DIFFERENT failure code is news.
  TEST_ASSERT_EQUAL(BOOT_POLL_FAILED,
                    bootStage2Poll(during(BOOT_STATE_PAGE7_INSTALLED,
                                          BOOT_RESULT_VERIFY_FAILED),
                                   BOOT_RESULT_REFUSED_BUSY));
  // ...and the state still wins over a stale code.
  TEST_ASSERT_EQUAL(BOOT_POLL_DONE,
                    bootStage2Poll(during(BOOT_STATE_NEW, BOOT_RESULT_REFUSED_BUSY),
                                   BOOT_RESULT_REFUSED_BUSY));
}

static void test_stage2_poll_waits_while_nothing_is_conclusive() {
  TEST_ASSERT_EQUAL(BOOT_POLL_WAIT,
                    bootStage2Poll(during(BOOT_STATE_PAGE7_INSTALLED, BOOT_RESULT_NONE),
                                   BOOT_RESULT_NONE));
  TEST_ASSERT_EQUAL(BOOT_POLL_WAIT,
                    bootStage2Poll(during(BOOT_STATE_TRAMPOLINE, BOOT_RESULT_NONE),
                                   BOOT_RESULT_NONE));
}

// --- did stage 1 start? (#516) ---

// Runs the helper against a scripted series of report reads (true = a valid
// report came back) and records the pauses it asked for.
struct StartScript {
  const bool* reads;
  int count;
  int used = 0;
  int pauses[8];
  int pauseCount = 0;
};

static bool runStart(StartScript& s) {
  return bootStage1WentOffBus(
      [&]() {
        TEST_ASSERT_TRUE_MESSAGE(s.used < s.count, "more reads than scripted");
        return s.reads[s.used++];
      },
      [&](uint16_t ms) { s.pauses[s.pauseCount++] = ms; });
}

static void test_a_unit_that_keeps_answering_never_started() {
  const bool reads[] = {true, true};
  StartScript s{reads, 2};
  TEST_ASSERT_FALSE(runStart(s));
  TEST_ASSERT_EQUAL_INT(2, s.used);
  TEST_ASSERT_EQUAL_INT(200, s.pauses[0]);
  TEST_ASSERT_EQUAL_INT(300, s.pauses[1]);
}

static void test_a_unit_off_the_bus_started() {
  const bool reads[] = {false, false};  // probe, then its confirmation
  StartScript s{reads, 2};
  TEST_ASSERT_TRUE(runStart(s));
  TEST_ASSERT_EQUAL_INT(2, s.used);
  TEST_ASSERT_EQUAL_INT(200, s.pauses[0]);
  TEST_ASSERT_EQUAL_INT(50, s.pauses[1]);
}

static void test_one_corrupted_reply_is_not_an_absence() {
  // A refusing unit whose first reply is garbled: the confirmation read gets
  // through, and so does the second probe.
  const bool reads[] = {false, true, true};
  StartScript s{reads, 3};
  TEST_ASSERT_FALSE(runStart(s));
  TEST_ASSERT_EQUAL_INT(3, s.used);
}

static void test_going_off_the_bus_at_the_second_probe_still_counts() {
  const bool reads[] = {true, false, false};
  StartScript s{reads, 3};
  TEST_ASSERT_TRUE(runStart(s));
  TEST_ASSERT_EQUAL_INT(3, s.used);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_new_exits_immediately);
  RUN_TEST(test_new_exits_before_lock_check);
  RUN_TEST(test_old_with_open_lock_needs_both_stages);
  RUN_TEST(test_old_with_locked_boot_section_refuses);
  RUN_TEST(test_page7_installed_needs_stage2_only);
  RUN_TEST(test_trampoline_needs_stage2_only);
  RUN_TEST(test_unknown_state_refuses);
  RUN_TEST(test_page7_installed_with_locked_boot_refuses);
  RUN_TEST(test_unreadable_lock_proceeds_in_every_updatable_state);
  RUN_TEST(test_unreadable_lock_does_not_rescue_an_unknown_state);
  RUN_TEST(test_result_failure_mapping);
  RUN_TEST(test_stage2_poll_state_new_is_done_whatever_the_result);
  RUN_TEST(test_stage2_poll_fresh_failure_ends_the_poll);
  RUN_TEST(test_stage2_poll_ignores_a_result_that_was_there_before_the_send);
  RUN_TEST(test_stage2_poll_waits_while_nothing_is_conclusive);
  RUN_TEST(test_a_unit_that_keeps_answering_never_started);
  RUN_TEST(test_a_unit_off_the_bus_started);
  RUN_TEST(test_one_corrupted_reply_is_not_an_absence);
  RUN_TEST(test_going_off_the_bus_at_the_second_probe_still_counts);
  return UNITY_END();
}
