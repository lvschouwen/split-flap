// Native tests for BootGuardPolicy.h: the count of crashes in a row that
// sends a board to its rescue image.
#include <unity.h>

#include <string.h>

#include "BootGuardPolicy.h"
#include "CrashContextPolicy.h"  // CrashReset: the reset reasons by number

void setUp() {}
void tearDown() {}

static uint8_t afterResets(const int* reasons, int n) {
  uint8_t crashes = 0;
  for (int i = 0; i < n; i++) crashes = bootGuardStep(crashes, reasons[i]);
  return crashes;
}

static void test_garbage_after_power_on_reads_as_no_crashes() {
  BootGuardRecord r;
  memset(&r, 0xA5, sizeof(r));
  TEST_ASSERT_EQUAL(0, bootGuardDecode(r));
}

static void test_record_round_trips_and_a_flipped_count_is_dropped() {
  BootGuardRecord r;
  bootGuardEncode(r, 2);
  TEST_ASSERT_EQUAL(2, bootGuardDecode(r));
  r.crashes = 7;  // the count changed without its complement
  TEST_ASSERT_EQUAL(0, bootGuardDecode(r));
}

static void test_every_crash_reset_counts() {
  TEST_ASSERT_EQUAL(1, bootGuardStep(0, CRASH_RESET_PANIC));
  TEST_ASSERT_EQUAL(1, bootGuardStep(0, CRASH_RESET_INT_WDT));
  TEST_ASSERT_EQUAL(1, bootGuardStep(0, CRASH_RESET_TASK_WDT));
  TEST_ASSERT_EQUAL(1, bootGuardStep(0, CRASH_RESET_WDT));
  TEST_ASSERT_EQUAL(1, bootGuardStep(0, CRASH_RESET_CPU_LOCKUP));
}

static void test_a_restart_on_purpose_neither_counts_nor_forgives() {
  TEST_ASSERT_EQUAL(0, bootGuardStep(0, CRASH_RESET_SW));
  TEST_ASSERT_EQUAL(2, bootGuardStep(2, CRASH_RESET_SW));
}

static void test_a_brownout_neither_counts_nor_forgives() {
  // A sagging supply is not a broken image: the rescue image cannot fix it.
  TEST_ASSERT_EQUAL(0, bootGuardStep(0, CRASH_RESET_BROWNOUT));
  TEST_ASSERT_EQUAL(2, bootGuardStep(2, CRASH_RESET_BROWNOUT));
}

static void test_power_on_starts_over() {
  TEST_ASSERT_EQUAL(0, bootGuardStep(2, CRASH_RESET_POWERON));
}

static void test_the_count_saturates() {
  TEST_ASSERT_EQUAL(255, bootGuardStep(255, CRASH_RESET_PANIC));
}

static void test_trips_at_the_limit_and_not_before() {
  const int two[] = {CRASH_RESET_PANIC, CRASH_RESET_PANIC};
  TEST_ASSERT_FALSE(bootGuardShouldTrip(afterResets(two, 2)));
  const int three[] = {CRASH_RESET_PANIC, CRASH_RESET_TASK_WDT, CRASH_RESET_PANIC};
  TEST_ASSERT_TRUE(bootGuardShouldTrip(afterResets(three, 3)));
  TEST_ASSERT_TRUE(bootGuardShouldTrip(255));  // still due while it cannot be armed
}

static void test_crashes_around_restarts_on_purpose_still_trip() {
  const int mixed[] = {CRASH_RESET_PANIC, CRASH_RESET_SW, CRASH_RESET_PANIC,
                       CRASH_RESET_BROWNOUT, CRASH_RESET_PANIC};
  TEST_ASSERT_TRUE(bootGuardShouldTrip(afterResets(mixed, 5)));
}

static void test_healthy_only_after_the_full_time() {
  TEST_ASSERT_FALSE(bootGuardHealthy(0));
  TEST_ASSERT_FALSE(bootGuardHealthy(BOOT_GUARD_HEALTHY_MS - 1));
  TEST_ASSERT_TRUE(bootGuardHealthy(BOOT_GUARD_HEALTHY_MS));
}

static void test_healthy_time_outlasts_the_task_watchdog() {
  // A boot that hangs is reset by the 30 s task watchdog; it must not have
  // been called healthy first.
  TEST_ASSERT_TRUE(BOOT_GUARD_HEALTHY_MS > 30000UL);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_garbage_after_power_on_reads_as_no_crashes);
  RUN_TEST(test_record_round_trips_and_a_flipped_count_is_dropped);
  RUN_TEST(test_every_crash_reset_counts);
  RUN_TEST(test_a_restart_on_purpose_neither_counts_nor_forgives);
  RUN_TEST(test_a_brownout_neither_counts_nor_forgives);
  RUN_TEST(test_power_on_starts_over);
  RUN_TEST(test_the_count_saturates);
  RUN_TEST(test_trips_at_the_limit_and_not_before);
  RUN_TEST(test_crashes_around_restarts_on_purpose_still_trip);
  RUN_TEST(test_healthy_only_after_the_full_time);
  RUN_TEST(test_healthy_time_outlasts_the_task_watchdog);
  return UNITY_END();
}
