// Host-side unit tests for the pure master-side drift-event log decision in
// DriftLogPolicy.h (#322): turning the unit's monotonic saturating drift-event
// counter (de) into "how many NEW events since we last looked", robustly across
// probe re-baselines and transient diag-read gaps. The SerialPrintf glue is
// bench tier.

#include <unity.h>
#include <stdint.h>
#include "../../DriftLogPolicy.h"

void setUp() {}
void tearDown() {}

static void test_first_read_establishes_baseline_silently() {
  // baseline < 0: a unit's since-boot drift history (de=5) is NOT logged as
  // brand-new the first time we see it — just adopt it as the baseline.
  DriftLogDecision d = driftLogEvaluate(-1, 5);
  TEST_ASSERT_FALSE(d.shouldLog);
  TEST_ASSERT_EQUAL_INT16(5, d.newBaseline);
}

static void test_new_event_logs_the_delta() {
  DriftLogDecision d = driftLogEvaluate(5, 6);
  TEST_ASSERT_TRUE(d.shouldLog);
  TEST_ASSERT_EQUAL_UINT8(1, d.newEvents);
  TEST_ASSERT_EQUAL_INT16(6, d.newBaseline);
}

static void test_multiple_new_events_since_last_poll() {
  // A poll gap can straddle several drift events; log the count, not just one.
  DriftLogDecision d = driftLogEvaluate(2, 5);
  TEST_ASSERT_TRUE(d.shouldLog);
  TEST_ASSERT_EQUAL_UINT8(3, d.newEvents);
  TEST_ASSERT_EQUAL_INT16(5, d.newBaseline);
}

static void test_unchanged_counter_is_silent() {
  DriftLogDecision d = driftLogEvaluate(7, 7);
  TEST_ASSERT_FALSE(d.shouldLog);
  TEST_ASSERT_EQUAL_INT16(7, d.newBaseline);
}

static void test_counter_reset_rebaselines_silently() {
  // A unit reboot zeroes its de counter (cur < baseline). The reset itself is
  // not a drift event — re-baseline to the lower value, don't log.
  DriftLogDecision d = driftLogEvaluate(9, 0);
  TEST_ASSERT_FALSE(d.shouldLog);
  TEST_ASSERT_EQUAL_INT16(0, d.newBaseline);
}

static void test_baseline_zero_then_first_event() {
  // A unit seen at de=0 (no drift yet), then its first real event.
  DriftLogDecision base = driftLogEvaluate(-1, 0);
  TEST_ASSERT_FALSE(base.shouldLog);
  TEST_ASSERT_EQUAL_INT16(0, base.newBaseline);
  DriftLogDecision first = driftLogEvaluate(0, 1);
  TEST_ASSERT_TRUE(first.shouldLog);
  TEST_ASSERT_EQUAL_UINT8(1, first.newEvents);
}

static void test_saturated_counter_stops_logging() {
  // de saturates at 0xFF (UnitDrift.h): once pinned at 255 there are no more
  // increments to report, so a unit stuck at the ceiling goes quiet.
  DriftLogDecision d = driftLogEvaluate(255, 255);
  TEST_ASSERT_FALSE(d.shouldLog);
  TEST_ASSERT_EQUAL_INT16(255, d.newBaseline);
}

static void test_near_ceiling_still_reports() {
  DriftLogDecision d = driftLogEvaluate(254, 255);
  TEST_ASSERT_TRUE(d.shouldLog);
  TEST_ASSERT_EQUAL_UINT8(1, d.newEvents);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_first_read_establishes_baseline_silently);
  RUN_TEST(test_new_event_logs_the_delta);
  RUN_TEST(test_multiple_new_events_since_last_poll);
  RUN_TEST(test_unchanged_counter_is_silent);
  RUN_TEST(test_counter_reset_rebaselines_silently);
  RUN_TEST(test_baseline_zero_then_first_event);
  RUN_TEST(test_saturated_counter_stops_logging);
  RUN_TEST(test_near_ceiling_still_reports);
  return UNITY_END();
}
