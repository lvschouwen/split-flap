// Native tests for CrashContextPolicy.h: the per-task "what am I doing" record
// that survives a watchdog/panic reset in RTC memory.
#include <unity.h>

#include "CrashContextPolicy.h"

void setUp() {}
void tearDown() {}

static void test_fresh_context_is_invalid_until_armed() {
  CrashContext c;
  memset(&c, 0xA5, sizeof(c));  // RTC_NOINIT garbage after a power-on
  TEST_ASSERT_FALSE(crashCtxValid(c));
  crashCtxArm(c);
  TEST_ASSERT_TRUE(crashCtxValid(c));
  for (int i = 0; i < CRASH_CTX_SLOTS; i++) {
    TEST_ASSERT_EQUAL(CRASH_ACT_NONE, c.slot[i].act);
  }
}

static void test_set_records_activity_arg_and_time() {
  CrashContext c;
  crashCtxArm(c);
  crashCtxSet(c, CRASH_SLOT_DISPLAY, CRASH_ACT_I2C_READ, 0x07, 1234);
  TEST_ASSERT_EQUAL(CRASH_ACT_I2C_READ, c.slot[CRASH_SLOT_DISPLAY].act);
  TEST_ASSERT_EQUAL(0x07, c.slot[CRASH_SLOT_DISPLAY].arg);
  TEST_ASSERT_EQUAL_UINT32(1234, c.slot[CRASH_SLOT_DISPLAY].sinceMs);
  TEST_ASSERT_TRUE(crashCtxValid(c));  // checksum-free: set keeps it valid
}

static void test_repeating_the_same_activity_keeps_its_start_time() {
  // "stuck in X since t" must survive a loop that re-marks X every pass.
  CrashContext c;
  crashCtxArm(c);
  crashCtxSet(c, CRASH_SLOT_NET, CRASH_ACT_WEB_LOOP, 0, 100);
  crashCtxSet(c, CRASH_SLOT_NET, CRASH_ACT_WEB_LOOP, 0, 900);
  TEST_ASSERT_EQUAL_UINT32(100, c.slot[CRASH_SLOT_NET].sinceMs);
  crashCtxSet(c, CRASH_SLOT_NET, CRASH_ACT_WIFI, 0, 950);
  TEST_ASSERT_EQUAL_UINT32(950, c.slot[CRASH_SLOT_NET].sinceMs);
}

static void test_out_of_range_slot_is_ignored() {
  CrashContext c;
  crashCtxArm(c);
  crashCtxSet(c, CRASH_CTX_SLOTS, CRASH_ACT_FRAME, 0, 1);
  crashCtxSet(c, -1, CRASH_ACT_FRAME, 0, 1);
  TEST_ASSERT_TRUE(crashCtxValid(c));
}

static void test_age_uses_last_tick() {
  CrashContext c;
  crashCtxArm(c);
  crashCtxSet(c, CRASH_SLOT_DISPLAY, CRASH_ACT_I2C_WRITE, 3, 1000);
  crashCtxTick(c, 4500);
  TEST_ASSERT_EQUAL_UINT32(3500, crashCtxAgeMs(c, CRASH_SLOT_DISPLAY));
}

static void test_names() {
  TEST_ASSERT_EQUAL_STRING("display", crashSlotName(CRASH_SLOT_DISPLAY));
  TEST_ASSERT_EQUAL_STRING("i2c-read", crashActName(CRASH_ACT_I2C_READ));
  TEST_ASSERT_EQUAL_STRING("?", crashActName(200));
}

static void test_crash_reset_classification() {
  TEST_ASSERT_TRUE(crashCtxWorthReporting(CRASH_RESET_INT_WDT));
  TEST_ASSERT_TRUE(crashCtxWorthReporting(CRASH_RESET_TASK_WDT));
  TEST_ASSERT_TRUE(crashCtxWorthReporting(CRASH_RESET_PANIC));
  TEST_ASSERT_TRUE(crashCtxWorthReporting(CRASH_RESET_WDT));
  TEST_ASSERT_TRUE(crashCtxWorthReporting(CRASH_RESET_BROWNOUT));
  TEST_ASSERT_FALSE(crashCtxWorthReporting(CRASH_RESET_SW));
  TEST_ASSERT_FALSE(crashCtxWorthReporting(CRASH_RESET_POWERON));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_fresh_context_is_invalid_until_armed);
  RUN_TEST(test_set_records_activity_arg_and_time);
  RUN_TEST(test_repeating_the_same_activity_keeps_its_start_time);
  RUN_TEST(test_out_of_range_slot_is_ignored);
  RUN_TEST(test_age_uses_last_tick);
  RUN_TEST(test_names);
  RUN_TEST(test_crash_reset_classification);
  return UNITY_END();
}
