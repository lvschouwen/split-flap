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

// --- deaf-slave check (#502) ---

static const uint8_t kAddr = 0x05;
static const uint8_t kGoodTwar = (uint8_t)((kAddr << 1) | 1);

static void test_listen_config_intact_needs_all_three_twcr_bits() {
  TEST_ASSERT_TRUE(twiListenConfigIntact(0x45, kGoodTwar, kAddr));
  TEST_ASSERT_TRUE(twiListenConfigIntact(0xC5, kGoodTwar, kAddr));  // TWINT set
  TEST_ASSERT_FALSE(twiListenConfigIntact(0x05, kGoodTwar, kAddr)); // no TWEA
  TEST_ASSERT_FALSE(twiListenConfigIntact(0x41, kGoodTwar, kAddr)); // no TWEN
  TEST_ASSERT_FALSE(twiListenConfigIntact(0x44, kGoodTwar, kAddr)); // no TWIE
  TEST_ASSERT_FALSE(twiListenConfigIntact(0x00, kGoodTwar, kAddr));
}

static void test_listen_config_intact_checks_address_and_general_call() {
  TEST_ASSERT_FALSE(twiListenConfigIntact(0x45, (uint8_t)(kAddr << 1), kAddr));
  TEST_ASSERT_FALSE(
      twiListenConfigIntact(0x45, (uint8_t)(((kAddr + 1) << 1) | 1), kAddr));
  TEST_ASSERT_FALSE(twiListenConfigIntact(0x45, 0x00, kAddr));
}

static void test_intact_config_never_resets() {
  TwiDeafState s;
  for (uint32_t t = 0; t < 10000; t += 7) {
    TEST_ASSERT_FALSE(twiDeafShouldReset(s, true, true, t));
  }
}

static void test_transient_bad_config_never_resets() {
  // TWEA drops for a byte time at the end of every slave transmit.
  TwiDeafState s;
  for (uint32_t t = 0; t < 10000; t += 10) {
    TEST_ASSERT_FALSE(twiDeafShouldReset(s, false, true, t));
    TEST_ASSERT_FALSE(twiDeafShouldReset(s, true, true, t + 1));
  }
}

static void test_bad_config_resets_after_hold_window() {
  TwiDeafState s;
  TEST_ASSERT_FALSE(twiDeafShouldReset(s, false, true, 1000));
  TEST_ASSERT_FALSE(
      twiDeafShouldReset(s, false, true, 1000 + TWI_HEAL_HOLD_MS - 1));
  TEST_ASSERT_TRUE(twiDeafShouldReset(s, false, true, 1000 + TWI_HEAL_HOLD_MS));
}

static void test_deaf_reset_waits_for_free_lines() {
  // Bus traffic to other units must not restart the window, only defer the
  // reset to an instant with both lines high.
  TwiDeafState s;
  TEST_ASSERT_FALSE(twiDeafShouldReset(s, false, false, 0));
  TEST_ASSERT_FALSE(twiDeafShouldReset(s, false, false, TWI_HEAL_HOLD_MS));
  TEST_ASSERT_FALSE(twiDeafShouldReset(s, false, false, TWI_HEAL_HOLD_MS + 5));
  TEST_ASSERT_TRUE(twiDeafShouldReset(s, false, true, TWI_HEAL_HOLD_MS + 6));
}

static void test_deaf_cooldown_and_count() {
  TwiDeafState s;
  twiDeafShouldReset(s, false, true, 0);
  TEST_ASSERT_TRUE(twiDeafShouldReset(s, false, true, TWI_HEAL_HOLD_MS));
  twiDeafNoteReset(s, TWI_HEAL_HOLD_MS);
  TEST_ASSERT_EQUAL_UINT8(1, s.resets);
  // A re-init that did not stick: the cooldown bounds the retry rate.
  uint32_t t0 = TWI_HEAL_HOLD_MS;
  bool fired = false;
  for (uint32_t k = 1; k < TWI_HEAL_COOLDOWN_MS; k++) {
    fired |= twiDeafShouldReset(s, false, true, t0 + k);
  }
  TEST_ASSERT_FALSE(fired);
  TEST_ASSERT_TRUE(twiDeafShouldReset(s, false, true, t0 + TWI_HEAL_COOLDOWN_MS));
}

static void test_deaf_count_saturates() {
  TwiDeafState s;
  for (int i = 0; i < 300; i++) twiDeafNoteReset(s, (uint32_t)i * 2000);
  TEST_ASSERT_EQUAL_UINT8(0xFF, s.resets);
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
  RUN_TEST(test_listen_config_intact_needs_all_three_twcr_bits);
  RUN_TEST(test_listen_config_intact_checks_address_and_general_call);
  RUN_TEST(test_intact_config_never_resets);
  RUN_TEST(test_transient_bad_config_never_resets);
  RUN_TEST(test_bad_config_resets_after_hold_window);
  RUN_TEST(test_deaf_reset_waits_for_free_lines);
  RUN_TEST(test_deaf_cooldown_and_count);
  RUN_TEST(test_deaf_count_saturates);
  return UNITY_END();
}
