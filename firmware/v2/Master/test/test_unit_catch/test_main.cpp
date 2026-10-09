// Host-side tests for UnitCatchPolicy.h (#554): which unit a board remembers
// to catch in its bootloader at the next power-on, and what it does with it.

#include <unity.h>
#include <string.h>
#include "../../UnitCatchPolicy.h"

void setUp() {}
void tearDown() {}

static UnitFacts unit(uint8_t state, const char* version) {
  UnitFacts u;
  u.state = state;
  strncpy(u.version, version, sizeof(u.version) - 1);
  return u;
}

static void test_a_forced_run_the_unit_did_not_enter_arms_its_address() {
  TEST_ASSERT_EQUAL_UINT8(6, unitCatchAfterForcedRun(0, true, false, 1, 6));
}

static void test_nothing_else_arms() {
  TEST_ASSERT_EQUAL_UINT8(0, unitCatchAfterForcedRun(0, false, false, 1, 6));  // a row run
  TEST_ASSERT_EQUAL_UINT8(0, unitCatchAfterForcedRun(0, true, true, 1, 6));    // stopped
  TEST_ASSERT_EQUAL_UINT8(0, unitCatchAfterForcedRun(0, true, false, 0, 6));   // it entered
  // And what was armed stays armed through a run that says nothing.
  TEST_ASSERT_EQUAL_UINT8(4, unitCatchAfterForcedRun(4, true, false, 0, 6));
}

static void test_only_a_row_address_is_ever_asked() {
  TEST_ASSERT_TRUE(unitCatchAddressValid(1, 1, 16));
  TEST_ASSERT_TRUE(unitCatchAddressValid(16, 1, 16));
  TEST_ASSERT_FALSE(unitCatchAddressValid(0, 1, 16));    // general call
  TEST_ASSERT_FALSE(unitCatchAddressValid(17, 1, 16));
  TEST_ASSERT_FALSE(unitCatchAddressValid(0xFF, 1, 16)); // blank NVS byte
}

static void test_a_caught_unit_is_flashed() {
  TEST_ASSERT_TRUE(UnitCatchStep::Flash == unitCatchAfterProbe(6, unit(2, "")));
}

static void test_a_unit_that_reads_again_ends_the_catch() {
  TEST_ASSERT_TRUE(UnitCatchStep::Disarm == unitCatchAfterProbe(6, unit(1, "3636a95")));
}

// The program that no longer listens ACKs its address with a version that
// cannot be read, or does not ACK at all: neither ends the catch.
static void test_a_unit_that_still_cannot_be_read_stays_armed() {
  TEST_ASSERT_TRUE(UnitCatchStep::Nothing == unitCatchAfterProbe(6, unit(1, "")));
  TEST_ASSERT_TRUE(UnitCatchStep::Nothing == unitCatchAfterProbe(6, unit(0, "")));
}

static void test_not_armed_does_nothing() {
  TEST_ASSERT_TRUE(UnitCatchStep::Nothing == unitCatchAfterProbe(0, unit(2, "")));
}

// The questions cover the bootloader's one second with room on both sides.
static void test_the_window_covers_the_bootloaders_second() {
  TEST_ASSERT_TRUE(UNIT_CATCH_WINDOW_MS > 1000);
  TEST_ASSERT_TRUE(UNIT_CATCH_WINDOW_MS <= 2000);  // inside setup()'s own delay
  TEST_ASSERT_TRUE(UNIT_CATCH_GAP_MS * 20 <= 1000);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_a_forced_run_the_unit_did_not_enter_arms_its_address);
  RUN_TEST(test_nothing_else_arms);
  RUN_TEST(test_only_a_row_address_is_ever_asked);
  RUN_TEST(test_a_caught_unit_is_flashed);
  RUN_TEST(test_a_unit_that_reads_again_ends_the_catch);
  RUN_TEST(test_a_unit_that_still_cannot_be_read_stays_armed);
  RUN_TEST(test_not_armed_does_nothing);
  RUN_TEST(test_the_window_covers_the_bootloaders_second);
  return UNITY_END();
}
