// Host-side tests for FollowerEscalation.h (#503): when the ESP-01 row stops
// waiting for a fault to heal and restarts itself, and how often it may.

#include <unity.h>

#include "../../FollowerEscalation.h"

void setUp() {}
void tearDown() {}

static EscalationInput healthy(uint32_t nowMs) {
  EscalationInput in;
  in.nowMs = nowMs;
  in.unitsSeenThisBoot = true;
  return in;
}

static void test_a_healthy_row_never_escalates() {
  EscalationState st;
  for (uint32_t t = 0; t < 86400000UL; t += 60000UL) {
    TEST_ASSERT_TRUE(EscalationCause::None == escalationStep(st, healthy(t)).cause);
  }
}

static void test_bus_dead_escalates_after_ten_minutes() {
  EscalationState st;
  EscalationInput in = healthy(5000 + ESCALATION_BUS_DEAD_MS - 1);
  in.busDead = true;
  in.busDeadSinceMs = 5000;
  TEST_ASSERT_TRUE(EscalationCause::None == escalationStep(st, in).cause);
  in.nowMs = 5000 + ESCALATION_BUS_DEAD_MS;
  TEST_ASSERT_TRUE(EscalationCause::BusDead == escalationStep(st, in).cause);
}

// Nothing plugged in, or a fault that survived the restart: a bus that has
// had no unit answer this boot is not one a restart can bring back.
static void test_a_row_that_never_saw_a_unit_is_not_restarted() {
  EscalationState st;
  EscalationInput in = healthy(24UL * 3600UL * 1000UL);
  in.busDead = true;
  in.busDeadSinceMs = 1000;
  in.unitsSeenThisBoot = false;
  TEST_ASSERT_TRUE(EscalationCause::None == escalationStep(st, in).cause);
}

// The recovery leaves deadSinceMs behind when an episode closes: an old
// start time on a bus that is alive means nothing.
static void test_a_stale_death_time_on_a_live_bus_is_ignored() {
  EscalationState st;
  EscalationInput in = healthy(10UL * 3600UL * 1000UL);
  in.busDead = false;
  in.busDeadSinceMs = 1000;
  TEST_ASSERT_TRUE(EscalationCause::None == escalationStep(st, in).cause);
}

static void test_low_heap_is_logged_once_then_escalates() {
  EscalationState st;
  EscalationInput in = healthy(1000);
  in.largestFreeBlock = ESCALATION_LOW_BLOCK_BYTES - 1;
  TEST_ASSERT_FALSE(escalationStep(st, in).logLowHeap);
  in.nowMs = 1000 + ESCALATION_LOW_HEAP_LOG_MS;
  TEST_ASSERT_TRUE(escalationStep(st, in).logLowHeap);
  in.nowMs = 2000 + ESCALATION_LOW_HEAP_LOG_MS;
  TEST_ASSERT_FALSE(escalationStep(st, in).logLowHeap);  // once per episode
  in.nowMs = 1000 + ESCALATION_LOW_HEAP_MS - 1;
  TEST_ASSERT_TRUE(EscalationCause::None == escalationStep(st, in).cause);
  in.nowMs = 1000 + ESCALATION_LOW_HEAP_MS;
  TEST_ASSERT_TRUE(EscalationCause::LowHeap == escalationStep(st, in).cause);
  // One sample at the floor ends the episode; the next one is logged again.
  in.largestFreeBlock = ESCALATION_LOW_BLOCK_BYTES;
  TEST_ASSERT_TRUE(EscalationCause::None == escalationStep(st, in).cause);
  in.largestFreeBlock = 100;
  in.nowMs += 1000;
  escalationStep(st, in);
  in.nowMs += ESCALATION_LOW_HEAP_LOG_MS;
  TEST_ASSERT_TRUE(escalationStep(st, in).logLowHeap);
}

// Low heap is the one that takes the board down by itself: it is named first.
static void test_low_heap_outranks_a_dead_bus() {
  EscalationState st;
  EscalationInput in = healthy(1000);
  in.busDead = true;
  in.busDeadSinceMs = 1000;
  in.largestFreeBlock = 100;
  escalationStep(st, in);
  in.nowMs = 1000 + ESCALATION_BUS_DEAD_MS;
  TEST_ASSERT_TRUE(EscalationCause::LowHeap == escalationStep(st, in).cause);
}

static void test_clocks_survive_millis_wrap() {
  EscalationState st;
  EscalationInput in = healthy(0xFFFFF000UL);
  in.busDead = true;
  in.busDeadSinceMs = 0xFFFFF000UL;
  in.nowMs = 0xFFFFF000UL + ESCALATION_BUS_DEAD_MS - 1;
  TEST_ASSERT_TRUE(EscalationCause::None == escalationStep(st, in).cause);
  in.nowMs = 0xFFFFF000UL + ESCALATION_BUS_DEAD_MS;
  TEST_ASSERT_TRUE(EscalationCause::BusDead == escalationStep(st, in).cause);
}

// --- the rate limit --------------------------------------------------------------

static void test_no_record_allows_the_first_escalation() {
  EscalationRecord garbage;
  garbage.magic = 0x12345678UL;
  garbage.minutesSince = 3;
  garbage.countAndCause = 0xFFFFFFFFUL;
  TEST_ASSERT_FALSE(escalationRecordValid(garbage));
  TEST_ASSERT_TRUE(escalationAllowed(garbage));
  TEST_ASSERT_EQUAL_UINT32(0, escalationCount(garbage));
  EscalationRecord fresh;
  TEST_ASSERT_TRUE(escalationAllowed(fresh));
}

static void test_one_escalation_per_six_hours_of_uptime() {
  EscalationRecord r;
  escalationRecordTaken(r, EscalationCause::BusDead);
  TEST_ASSERT_TRUE(escalationRecordValid(r));
  TEST_ASSERT_EQUAL_UINT32(1, escalationCount(r));
  TEST_ASSERT_EQUAL_STRING("bus-dead", escalationCauseName(escalationLastCause(r)));
  TEST_ASSERT_FALSE(escalationAllowed(r));
  for (unsigned m = 0; m < ESCALATION_MIN_INTERVAL_MIN - 1; m++) {
    escalationRecordMinute(r);
  }
  TEST_ASSERT_FALSE(escalationAllowed(r));
  escalationRecordMinute(r);
  TEST_ASSERT_TRUE(escalationAllowed(r));
  escalationRecordTaken(r, EscalationCause::LowHeap);
  TEST_ASSERT_EQUAL_UINT32(2, escalationCount(r));
  TEST_ASSERT_EQUAL_STRING("low-heap", escalationCauseName(escalationLastCause(r)));
  TEST_ASSERT_FALSE(escalationAllowed(r));
}

// Minutes counted on a board that never escalated must not turn the first
// record into a rate-limited one.
static void test_minutes_before_any_escalation_do_not_limit() {
  EscalationRecord r;
  for (int m = 0; m < 5; m++) escalationRecordMinute(r);
  TEST_ASSERT_TRUE(escalationRecordValid(r));
  TEST_ASSERT_EQUAL_UINT32(0, escalationCount(r));
  TEST_ASSERT_TRUE(escalationAllowed(r));
}

static void test_a_corrupted_record_starts_over() {
  EscalationRecord r;
  escalationRecordTaken(r, EscalationCause::LowHeap);
  r.minutesSince ^= 0x10;  // one flipped bit
  TEST_ASSERT_FALSE(escalationRecordValid(r));
  TEST_ASSERT_TRUE(escalationAllowed(r));
  escalationRecordMinute(r);
  TEST_ASSERT_TRUE(escalationRecordValid(r));
  TEST_ASSERT_EQUAL_UINT32(1, r.minutesSince);
  TEST_ASSERT_EQUAL_UINT32(0, escalationCount(r));
}

static void test_the_uptime_counter_saturates() {
  EscalationRecord r;
  escalationRecordTaken(r, EscalationCause::BusDead);
  r.minutesSince = 0xFFFFFFFFUL;
  escalationRecordSeal(r);
  escalationRecordMinute(r);
  TEST_ASSERT_EQUAL_UINT32(0xFFFFFFFFUL, r.minutesSince);
  TEST_ASSERT_TRUE(escalationAllowed(r));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_a_healthy_row_never_escalates);
  RUN_TEST(test_bus_dead_escalates_after_ten_minutes);
  RUN_TEST(test_a_row_that_never_saw_a_unit_is_not_restarted);
  RUN_TEST(test_a_stale_death_time_on_a_live_bus_is_ignored);
  RUN_TEST(test_low_heap_is_logged_once_then_escalates);
  RUN_TEST(test_low_heap_outranks_a_dead_bus);
  RUN_TEST(test_clocks_survive_millis_wrap);
  RUN_TEST(test_no_record_allows_the_first_escalation);
  RUN_TEST(test_one_escalation_per_six_hours_of_uptime);
  RUN_TEST(test_minutes_before_any_escalation_do_not_limit);
  RUN_TEST(test_a_corrupted_record_starts_over);
  RUN_TEST(test_the_uptime_counter_saturates);
  return UNITY_END();
}
