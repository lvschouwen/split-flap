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
  return UNITY_END();
}
