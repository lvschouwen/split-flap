// Host-side unit tests for the display-width derivation (#123).
// computeDisplayWidth() turns the boot-time I2C probe results into the
// effective character width every text-layout helper targets.

#include <unity.h>
#include "DisplayWidth.h"

void setUp() {}
void tearDown() {}

#define MAX_UNITS 16

static void test_all_silent_falls_back_to_max() {
  int states[MAX_UNITS] = {0};
  TEST_ASSERT_EQUAL_INT(MAX_UNITS, computeDisplayWidth(states, MAX_UNITS));
}

static void test_contiguous_units_width_matches_count() {
  int states[MAX_UNITS] = {0};
  for (int i = 0; i < 5; i++) states[i] = 1;
  TEST_ASSERT_EQUAL_INT(5, computeDisplayWidth(states, MAX_UNITS));
}

static void test_gap_mid_display_does_not_shrink_width() {
  // Units 0,1 alive, 2..3 dead, 4 alive: a dead unit mid-display keeps its
  // slot — width is highest index + 1, not the responder count.
  int states[MAX_UNITS] = {0};
  states[0] = 1;
  states[1] = 1;
  states[4] = 1;
  TEST_ASSERT_EQUAL_INT(5, computeDisplayWidth(states, MAX_UNITS));
}

static void test_bootloader_state_counts_as_present() {
  int states[MAX_UNITS] = {0};
  states[0] = 1;
  states[3] = 2;  // in twiboot, awaiting auto-install — physically present
  TEST_ASSERT_EQUAL_INT(4, computeDisplayWidth(states, MAX_UNITS));
}

static void test_single_unit_at_top_address_gives_full_width() {
  int states[MAX_UNITS] = {0};
  states[MAX_UNITS - 1] = 1;
  TEST_ASSERT_EQUAL_INT(MAX_UNITS, computeDisplayWidth(states, MAX_UNITS));
}

static void test_single_unit_display() {
  int states[MAX_UNITS] = {0};
  states[0] = 1;
  TEST_ASSERT_EQUAL_INT(1, computeDisplayWidth(states, MAX_UNITS));
}

// countRespondingUnits (#132): counts live units, unlike width it does NOT
// span gaps.
static void test_responders_none() {
  int states[MAX_UNITS] = {0};
  TEST_ASSERT_EQUAL_INT(0, countRespondingUnits(states, MAX_UNITS));
}
static void test_responders_contiguous() {
  int states[MAX_UNITS] = {0};
  for (int i = 0; i < 5; i++) states[i] = 1;
  TEST_ASSERT_EQUAL_INT(5, countRespondingUnits(states, MAX_UNITS));
}
static void test_responders_gap_counts_only_live() {
  int states[MAX_UNITS] = {0};
  states[0] = 1; states[1] = 1; states[4] = 2;  // gap at 2,3
  TEST_ASSERT_EQUAL_INT(3, countRespondingUnits(states, MAX_UNITS));   // vs width 5
  TEST_ASSERT_EQUAL_INT(5, computeDisplayWidth(states, MAX_UNITS));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_all_silent_falls_back_to_max);
  RUN_TEST(test_responders_none);
  RUN_TEST(test_responders_contiguous);
  RUN_TEST(test_responders_gap_counts_only_live);
  RUN_TEST(test_contiguous_units_width_matches_count);
  RUN_TEST(test_gap_mid_display_does_not_shrink_width);
  RUN_TEST(test_bootloader_state_counts_as_present);
  RUN_TEST(test_single_unit_at_top_address_gives_full_width);
  RUN_TEST(test_single_unit_display);
  return UNITY_END();
}
