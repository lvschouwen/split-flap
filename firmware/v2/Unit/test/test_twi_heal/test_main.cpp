// Host-side tests for the unit's TWI self-heal policy (UnitTwiHeal.h, #489):
// SDA held low past the hold window arms a peripheral reset, cooldown bounds
// the rate, and only a release that follows OUR reset is counted — every unit
// on the bus sees the same low line, and the count must name the culprit.

#include <unity.h>
#include <stdint.h>
#include "UnitTwiHeal.h"
#include "UnitExtDiag.h"

void setUp() {}
void tearDown() {}

static void test_high_line_never_resets() {
  TwiHealState s;
  for (uint32_t t = 0; t < 10000; t += 7) {
    TEST_ASSERT_FALSE(twiHealShouldReset(s, false, t));
  }
}

static void test_short_low_pulses_never_reset() {
  // Ordinary traffic: SDA low for a bit time, then high again.
  TwiHealState s;
  for (uint32_t t = 0; t < 10000; t += 10) {
    TEST_ASSERT_FALSE(twiHealShouldReset(s, true, t));
    TEST_ASSERT_FALSE(twiHealShouldReset(s, false, t + 1));
  }
}

static void test_held_low_resets_after_hold_window() {
  TwiHealState s;
  TEST_ASSERT_FALSE(twiHealShouldReset(s, true, 1000));
  TEST_ASSERT_FALSE(twiHealShouldReset(s, true, 1000 + TWI_HEAL_HOLD_MS - 1));
  TEST_ASSERT_TRUE(twiHealShouldReset(s, true, 1000 + TWI_HEAL_HOLD_MS));
}

static void test_cooldown_bounds_the_rate() {
  TwiHealState s;
  twiHealShouldReset(s, true, 0);
  TEST_ASSERT_TRUE(twiHealShouldReset(s, true, TWI_HEAL_HOLD_MS));
  twiHealNoteReset(s, TWI_HEAL_HOLD_MS, false);
  // Still held by someone else: a fresh hold window is not enough, the
  // cooldown must pass too.
  uint32_t t = TWI_HEAL_HOLD_MS;
  bool fired = false;
  for (uint32_t k = 1; k < TWI_HEAL_COOLDOWN_MS; k++) {
    fired |= twiHealShouldReset(s, true, t + k);
  }
  TEST_ASSERT_FALSE(fired);
  TEST_ASSERT_TRUE(twiHealShouldReset(s, true, t + TWI_HEAL_COOLDOWN_MS));
}

static void test_only_self_release_counts() {
  TwiHealState s;
  twiHealNoteReset(s, 100, false);  // line stayed low: not us
  TEST_ASSERT_EQUAL_UINT8(0, s.selfResets);
  twiHealNoteReset(s, 5000, true);  // line came free when we let go: us
  TEST_ASSERT_EQUAL_UINT8(1, s.selfResets);
}

static void test_self_count_saturates() {
  TwiHealState s;
  for (int i = 0; i < 400; i++) twiHealNoteReset(s, (uint32_t)i * 2000, true);
  TEST_ASSERT_EQUAL_UINT8(0xFF, s.selfResets);
}

static void test_survives_millis_wrap() {
  TwiHealState s;
  uint32_t base = 0xFFFFFFF0UL;
  TEST_ASSERT_FALSE(twiHealShouldReset(s, true, base));
  TEST_ASSERT_TRUE(twiHealShouldReset(s, true, base + TWI_HEAL_HOLD_MS));
}

static void test_status_bits_carry_count_beside_stall() {
  uint8_t bits = extDiagWithTwiHeal(EXT_DIAG_STATUS_STALL, 3);
  TEST_ASSERT_TRUE(bits & EXT_DIAG_STATUS_STALL);
  TEST_ASSERT_EQUAL_UINT8(3, extDiagTwiHealCount(bits));
  // Saturates at the 3-bit field, never spills into the other bits.
  bits = extDiagWithTwiHeal(0, 200);
  TEST_ASSERT_EQUAL_UINT8(7, extDiagTwiHealCount(bits));
  TEST_ASSERT_EQUAL_UINT8(0, bits & ~EXT_DIAG_STATUS_TWI_HEAL_MASK);
  // A stale count in the input is replaced, not OR-ed.
  bits = extDiagWithTwiHeal(extDiagWithTwiHeal(0, 7), 1);
  TEST_ASSERT_EQUAL_UINT8(1, extDiagTwiHealCount(bits));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_high_line_never_resets);
  RUN_TEST(test_short_low_pulses_never_reset);
  RUN_TEST(test_held_low_resets_after_hold_window);
  RUN_TEST(test_cooldown_bounds_the_rate);
  RUN_TEST(test_only_self_release_counts);
  RUN_TEST(test_self_count_saturates);
  RUN_TEST(test_survives_millis_wrap);
  RUN_TEST(test_status_bits_carry_count_beside_stall);
  return UNITY_END();
}
