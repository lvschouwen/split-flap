// Host-side tests for the reset-cause rules in shared/UnitResetCause.h (#502):
// which lifetime counter a boot moves on the unit, and what a master reads
// back from GET_STATUS byte 1. Reading MCUSR/GPIOR0 and the EEPROM marker is
// bench tier.

#include <unity.h>
#include <stdint.h>
#include "UnitResetCause.h"

void setUp() {}
void tearDown() {}

static void test_each_single_cause_is_named() {
  TEST_ASSERT_EQUAL(UNIT_RESET_POWER_ON, unitResetClassify(UNIT_MCUSR_PORF, false));
  TEST_ASSERT_EQUAL(UNIT_RESET_BROWNOUT, unitResetClassify(UNIT_MCUSR_BORF, false));
  TEST_ASSERT_EQUAL(UNIT_RESET_WATCHDOG, unitResetClassify(UNIT_MCUSR_WDRF, false));
  TEST_ASSERT_EQUAL(UNIT_RESET_EXTERNAL, unitResetClassify(UNIT_MCUSR_EXTRF, false));
  TEST_ASSERT_EQUAL(UNIT_RESET_UNKNOWN, unitResetClassify(0, false));
}

// A slow rail sets BORF together with PORF on an ordinary power-up; counting
// that would bump the brownout counter on every power cycle.
static void test_power_on_is_not_a_brownout() {
  TEST_ASSERT_EQUAL(UNIT_RESET_POWER_ON,
                    unitResetClassify(UNIT_MCUSR_PORF | UNIT_MCUSR_BORF, false));
  TEST_ASSERT_EQUAL(UNIT_RESET_POWER_ON,
                    unitResetClassify(UNIT_MCUSR_MASK, true));
}

static void test_a_requested_watchdog_reset_is_not_a_hang() {
  TEST_ASSERT_EQUAL(UNIT_RESET_REQUESTED, unitResetClassify(UNIT_MCUSR_WDRF, true));
  TEST_ASSERT_EQUAL(UNIT_RESET_WATCHDOG, unitResetClassify(UNIT_MCUSR_WDRF, false));
}

// The marker only explains a watchdog reset. Left over from a request that a
// brownout or a pin reset beat to it, it must not hide that cause.
static void test_the_marker_explains_nothing_but_a_watchdog_reset() {
  TEST_ASSERT_EQUAL(UNIT_RESET_BROWNOUT, unitResetClassify(UNIT_MCUSR_BORF, true));
  TEST_ASSERT_EQUAL(UNIT_RESET_BROWNOUT,
                    unitResetClassify(UNIT_MCUSR_BORF | UNIT_MCUSR_WDRF, true));
  TEST_ASSERT_EQUAL(UNIT_RESET_EXTERNAL, unitResetClassify(UNIT_MCUSR_EXTRF, true));
  TEST_ASSERT_EQUAL(UNIT_RESET_UNKNOWN, unitResetClassify(0, true));
}

static void test_status_byte_roundtrips_every_kind() {
  for (uint8_t mcusr = 0; mcusr <= UNIT_MCUSR_MASK; mcusr++) {
    for (int requested = 0; requested <= 1; requested++) {
      UnitResetKind kind = unitResetClassify(mcusr, requested != 0);
      uint8_t wire = unitResetStatusByte(mcusr, kind);
      TEST_ASSERT_EQUAL(kind, unitResetFromStatusByte(wire));
      TEST_ASSERT_EQUAL_UINT8(mcusr, wire & UNIT_MCUSR_MASK);
    }
  }
}

static void test_status_byte_flags_only_a_requested_reset() {
  TEST_ASSERT_EQUAL_UINT8(UNIT_MCUSR_WDRF | UNIT_RESET_REQUESTED_FLAG,
      unitResetStatusByte(UNIT_MCUSR_WDRF, UNIT_RESET_REQUESTED));
  TEST_ASSERT_EQUAL_UINT8(UNIT_MCUSR_WDRF,
      unitResetStatusByte(UNIT_MCUSR_WDRF, UNIT_RESET_WATCHDOG));
  // Bits above the four MCUSR flags never leak into the wire byte.
  TEST_ASSERT_EQUAL_UINT8(UNIT_MCUSR_BORF,
      unitResetStatusByte(0xF0 | UNIT_MCUSR_BORF, UNIT_RESET_BROWNOUT));
}

// A unit predating the flag sends the bare MCUSR bits (0 behind a bootloader
// that clears the register): that still decodes, never as "requested".
static void test_a_status_byte_without_the_flag_decodes_as_before() {
  TEST_ASSERT_EQUAL(UNIT_RESET_UNKNOWN, unitResetFromStatusByte(0));
  TEST_ASSERT_EQUAL(UNIT_RESET_WATCHDOG, unitResetFromStatusByte(UNIT_MCUSR_WDRF));
}

static void test_names() {
  TEST_ASSERT_EQUAL_STRING("power-on", unitResetKindName(UNIT_RESET_POWER_ON));
  TEST_ASSERT_EQUAL_STRING("brownout", unitResetKindName(UNIT_RESET_BROWNOUT));
  TEST_ASSERT_EQUAL_STRING("watchdog", unitResetKindName(UNIT_RESET_WATCHDOG));
  TEST_ASSERT_EQUAL_STRING("requested", unitResetKindName(UNIT_RESET_REQUESTED));
  TEST_ASSERT_EQUAL_STRING("external", unitResetKindName(UNIT_RESET_EXTERNAL));
  TEST_ASSERT_EQUAL_STRING("unknown", unitResetKindName(UNIT_RESET_UNKNOWN));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_each_single_cause_is_named);
  RUN_TEST(test_power_on_is_not_a_brownout);
  RUN_TEST(test_a_requested_watchdog_reset_is_not_a_hang);
  RUN_TEST(test_the_marker_explains_nothing_but_a_watchdog_reset);
  RUN_TEST(test_status_byte_roundtrips_every_kind);
  RUN_TEST(test_status_byte_flags_only_a_requested_reset);
  RUN_TEST(test_a_status_byte_without_the_flag_decodes_as_before);
  RUN_TEST(test_names);
  return UNITY_END();
}
