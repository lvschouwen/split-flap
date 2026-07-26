// Host-side tests for WearPolicy.h (#231) — relative wear assessment over
// the per-unit revolution odometers. A unit is flagged when it wears past
// max(WEAR_FLAG_RATIO x median, median + WEAR_FLAG_FLOOR_REVS); the floor
// stops false alarms on young displays where 2 x median is a tiny number.

#include <ArduinoFake.h>
#include <unity.h>

#include <cstring>

#include "WearPolicy.h"

void setUp() {}
void tearDown() {}

static void setOdo(UnitFacts& u, uint32_t revs) {
  u.odometer = revs;
  u.odometerValid = true;
}

// --- median ---------------------------------------------------------------

static void test_median_odd_count() {
  UnitFacts units[3];
  setOdo(units[0], 300);
  setOdo(units[1], 100);
  setOdo(units[2], 200);
  WearAssessment w;
  assessWear(units, 3, w);
  TEST_ASSERT_EQUAL_UINT32(200, w.median);
  TEST_ASSERT_EQUAL_INT(3, w.validCount);
}

static void test_median_even_count_takes_lower_middle() {
  // Lower middle, not upper: on a 2-unit display the worn unit must not be
  // able to set the median itself and escape its own flag.
  UnitFacts units[2];
  setOdo(units[0], 100);
  setOdo(units[1], 50000);
  WearAssessment w;
  assessWear(units, 2, w);
  TEST_ASSERT_EQUAL_UINT32(100, w.median);
}

static void test_ignores_units_without_valid_odometer() {
  UnitFacts units[3];
  setOdo(units[0], 400);
  units[1].odometer = 9999999;  // odometerValid stays false — old firmware
  setOdo(units[2], 600);
  WearAssessment w;
  assessWear(units, 3, w);
  TEST_ASSERT_EQUAL_INT(2, w.validCount);
  TEST_ASSERT_EQUAL_UINT32(400, w.median);
  TEST_ASSERT_FALSE(w.flagged[1]);
}

// --- flagging ---------------------------------------------------------------

static void test_flags_unit_wearing_past_double_median() {
  UnitFacts units[5];
  for (int i = 0; i < 4; i++) setOdo(units[i], 20000);
  setOdo(units[4], 50000);  // > max(2 x 20000, 20000 + 10000)
  WearAssessment w;
  assessWear(units, 5, w);
  TEST_ASSERT_TRUE(w.flagged[4]);
  TEST_ASSERT_EQUAL_INT(1, w.flaggedCount);
  TEST_ASSERT_FALSE(w.flagged[0]);
}

static void test_floor_suppresses_flags_on_young_display() {
  // 900 revs is 9 x the median but far below median + 10k — a brand-new
  // display exercising one unit more than the rest is not "worn".
  UnitFacts units[3];
  setOdo(units[0], 100);
  setOdo(units[1], 100);
  setOdo(units[2], 900);
  WearAssessment w;
  assessWear(units, 3, w);
  TEST_ASSERT_EQUAL_INT(0, w.flaggedCount);
}

static void test_exactly_at_threshold_is_not_flagged() {
  UnitFacts units[3];
  setOdo(units[0], 20000);
  setOdo(units[1], 20000);
  setOdo(units[2], 40000);  // == 2 x median, not >
  WearAssessment w;
  assessWear(units, 3, w);
  TEST_ASSERT_EQUAL_INT(0, w.flaggedCount);
}

static void test_no_valid_odometers_yields_empty_assessment() {
  UnitFacts units[4];
  WearAssessment w;
  assessWear(units, 4, w);
  TEST_ASSERT_EQUAL_INT(0, w.validCount);
  TEST_ASSERT_EQUAL_INT(0, w.flaggedCount);
  TEST_ASSERT_EQUAL_UINT32(0, w.median);
}

static void test_single_unit_never_flags_itself() {
  UnitFacts units[1];
  setOdo(units[0], 4000000);
  WearAssessment w;
  assessWear(units, 1, w);
  TEST_ASSERT_EQUAL_INT(0, w.flaggedCount);
}

// --- JSON fragment -----------------------------------------------------------
// Spliced into /units/health at the top level by WebEndpoints (same pattern
// as the #205 reflash progress object).

static void test_wear_json_fragment_shape() {
  UnitFacts units[4];
  for (int i = 0; i < 3; i++) setOdo(units[i], 20000);
  setOdo(units[3], 50000);
  WearAssessment w;
  assessWear(units, 4, w);
  char buf[96];
  size_t n = buildWearJson(w, buf, sizeof(buf));
  TEST_ASSERT_TRUE(n > 0 && n < sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("\"wear\":{\"median\":20000,\"flagged\":[3]}", buf);
}

static void test_wear_json_fragment_empty_flag_list() {
  UnitFacts units[2];
  setOdo(units[0], 10);
  setOdo(units[1], 12);
  WearAssessment w;
  assessWear(units, 2, w);
  char buf[64];
  size_t n = buildWearJson(w, buf, sizeof(buf));
  TEST_ASSERT_TRUE(n > 0 && n < sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("\"wear\":{\"median\":10,\"flagged\":[]}", buf);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_median_odd_count);
  RUN_TEST(test_median_even_count_takes_lower_middle);
  RUN_TEST(test_ignores_units_without_valid_odometer);
  RUN_TEST(test_flags_unit_wearing_past_double_median);
  RUN_TEST(test_floor_suppresses_flags_on_young_display);
  RUN_TEST(test_exactly_at_threshold_is_not_flagged);
  RUN_TEST(test_no_valid_odometers_yields_empty_assessment);
  RUN_TEST(test_single_unit_never_flags_itself);
  RUN_TEST(test_wear_json_fragment_shape);
  RUN_TEST(test_wear_json_fragment_empty_flag_list);
  UNITY_END();
  return 0;
}
