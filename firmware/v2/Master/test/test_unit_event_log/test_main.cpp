// Host-side unit tests for the pure unit-health event-transition detector in
// UnitEventLog.h (#322). The master surfaces per-unit health only as passive
// /units/health JSON; this turns the current per-unit condition mask into the
// onset/recovery TRANSITIONS worth one operator log line, robust across a unit
// going unreadable (unknown conditions must neither onset nor "recover"). The
// SerialPrintf glue + label strings are bench tier.

#include <unity.h>
#include <stdint.h>
#include "../../UnitEventLog.h"

void setUp() {}
void tearDown() {}

// Every condition observable unless a test says otherwise.
static const uint8_t ALL = UNIT_EVT_HOME_FAILED | UNIT_EVT_HALL_NEVER |
                           UNIT_EVT_STALE | UNIT_EVT_MISMATCH;

static void test_first_seen_fault_is_an_onset() {
  // Fresh baseline (0) + a unit already home-failed: log the onset once.
  UnitEventTransitions t = unitEventEvaluate(0, UNIT_EVT_HOME_FAILED, ALL);
  TEST_ASSERT_EQUAL_UINT8(UNIT_EVT_HOME_FAILED, t.onset);
  TEST_ASSERT_EQUAL_UINT8(0, t.recovery);
  TEST_ASSERT_EQUAL_UINT8(UNIT_EVT_HOME_FAILED, t.newState);
}

static void test_unchanged_state_is_silent() {
  UnitEventTransitions t =
      unitEventEvaluate(UNIT_EVT_STALE, UNIT_EVT_STALE, ALL);
  TEST_ASSERT_EQUAL_UINT8(0, t.onset);
  TEST_ASSERT_EQUAL_UINT8(0, t.recovery);
}

static void test_new_condition_onsets_without_disturbing_existing() {
  UnitEventTransitions t = unitEventEvaluate(
      UNIT_EVT_STALE, UNIT_EVT_STALE | UNIT_EVT_MISMATCH, ALL);
  TEST_ASSERT_EQUAL_UINT8(UNIT_EVT_MISMATCH, t.onset);
  TEST_ASSERT_EQUAL_UINT8(0, t.recovery);
}

static void test_recoverable_condition_clearing_logs_recovery() {
  // stale + home-failed are recoverable: clearing them is real good-news.
  UnitEventTransitions t = unitEventEvaluate(UNIT_EVT_STALE, 0, ALL);
  TEST_ASSERT_EQUAL_UINT8(0, t.onset);
  TEST_ASSERT_EQUAL_UINT8(UNIT_EVT_STALE, t.recovery);
  TEST_ASSERT_EQUAL_UINT8(0, t.newState);
}

static void test_nonrecoverable_condition_clears_silently() {
  // mismatch + hall-never are NOT recoverable: they re-baseline without a
  // recovery line (a mismatch clearing is usually the next frame catching up;
  // hall-never is effectively permanent).
  UnitEventTransitions t =
      unitEventEvaluate(UNIT_EVT_MISMATCH | UNIT_EVT_HALL_NEVER, 0, ALL);
  TEST_ASSERT_EQUAL_UINT8(0, t.onset);
  TEST_ASSERT_EQUAL_UINT8(0, t.recovery);
  TEST_ASSERT_EQUAL_UINT8(0, t.newState);
}

static void test_unreadable_unit_does_not_fake_recovery() {
  // A home-failed unit that goes unreadable (status bits no longer observable)
  // must NOT read as "home recovered". validMask drops HOME_FAILED/HALL_NEVER;
  // the prior state is carried, so no onset, no recovery, state preserved.
  uint8_t validMask = UNIT_EVT_STALE | UNIT_EVT_MISMATCH;  // status invalid
  UnitEventTransitions t =
      unitEventEvaluate(UNIT_EVT_HOME_FAILED, 0, validMask);
  TEST_ASSERT_EQUAL_UINT8(0, t.onset);
  TEST_ASSERT_EQUAL_UINT8(0, t.recovery);
  TEST_ASSERT_EQUAL_UINT8(UNIT_EVT_HOME_FAILED, t.newState);
}

static void test_going_stale_while_home_failed_logs_only_stale() {
  // Unit was home-failed (readable); now it dropped off the bus: STALE onsets,
  // HOME_FAILED is no longer observable so it's carried, not recovered.
  uint8_t validMask = UNIT_EVT_STALE | UNIT_EVT_MISMATCH;
  UnitEventTransitions t =
      unitEventEvaluate(UNIT_EVT_HOME_FAILED, UNIT_EVT_STALE, validMask);
  TEST_ASSERT_EQUAL_UINT8(UNIT_EVT_STALE, t.onset);
  TEST_ASSERT_EQUAL_UINT8(0, t.recovery);
  TEST_ASSERT_EQUAL_UINT8(UNIT_EVT_HOME_FAILED | UNIT_EVT_STALE, t.newState);
}

static void test_mismatch_diag_gap_carries_not_reonsets() {
  // mismatch rides the DIAG read; a missed diag this tick (mismatch not in
  // validMask, even while status stays valid) must CARRY a prior mismatch, not
  // drop it — else the next good read re-onsets it as a spurious fresh line
  // (#322 review HIGH). mismatch is non-recoverable, so no recovery line either.
  uint8_t validMask = UNIT_EVT_HOME_FAILED | UNIT_EVT_HALL_NEVER | UNIT_EVT_STALE;
  UnitEventTransitions t =
      unitEventEvaluate(UNIT_EVT_MISMATCH, 0, validMask);
  TEST_ASSERT_EQUAL_UINT8(0, t.onset);
  TEST_ASSERT_EQUAL_UINT8(0, t.recovery);
  TEST_ASSERT_EQUAL_UINT8(UNIT_EVT_MISMATCH, t.newState);
}

static void test_recoverable_mask_membership() {
  // Guard the policy: exactly home-failed + stale recover; hall-never, mismatch
  // and low-Vcc do not.
  TEST_ASSERT_TRUE(UNIT_EVT_RECOVERABLE & UNIT_EVT_HOME_FAILED);
  TEST_ASSERT_TRUE(UNIT_EVT_RECOVERABLE & UNIT_EVT_STALE);
  TEST_ASSERT_FALSE(UNIT_EVT_RECOVERABLE & UNIT_EVT_HALL_NEVER);
  TEST_ASSERT_FALSE(UNIT_EVT_RECOVERABLE & UNIT_EVT_MISMATCH);
  TEST_ASSERT_FALSE(UNIT_EVT_RECOVERABLE & UNIT_EVT_LOW_VCC);
}

// --- low-Vcc alert (#366) ---------------------------------------------------

static void test_low_vcc_onsets_once_and_stays_latched() {
  // A unit whose vccMin crossed the floor onsets LOW_VCC once (with all
  // conditions observable), then stays silent — vccMin only ever falls further.
  uint8_t all = ALL | UNIT_EVT_LOW_VCC;
  UnitEventTransitions t = unitEventEvaluate(0, UNIT_EVT_LOW_VCC, all);
  TEST_ASSERT_EQUAL_UINT8(UNIT_EVT_LOW_VCC, t.onset);
  TEST_ASSERT_EQUAL_UINT8(0, t.recovery);
  // Next tick, still low: no repeat line.
  t = unitEventEvaluate(UNIT_EVT_LOW_VCC, UNIT_EVT_LOW_VCC, all);
  TEST_ASSERT_EQUAL_UINT8(0, t.onset);
  TEST_ASSERT_EQUAL_UINT8(0, t.recovery);
}

static void test_low_vcc_vitals_gap_carries_no_recovery() {
  // vccMin rides the vitals read; a missed vitals poll (LOW_VCC not observable)
  // must CARRY a prior low-Vcc, never emit a phantom recovery (it's non-
  // recoverable) or re-onset when the next good read arrives.
  uint8_t validNoVitals = ALL;  // LOW_VCC bit absent
  UnitEventTransitions t = unitEventEvaluate(UNIT_EVT_LOW_VCC, 0, validNoVitals);
  TEST_ASSERT_EQUAL_UINT8(0, t.onset);
  TEST_ASSERT_EQUAL_UINT8(0, t.recovery);
  TEST_ASSERT_EQUAL_UINT8(UNIT_EVT_LOW_VCC, t.newState);
}

static void test_unit_vcc_is_low_threshold() {
  // Below the floor with valid vitals -> low.
  TEST_ASSERT_TRUE(unitVccIsLow(true, 3999, UNIT_VCC_MIN_FLOOR_MV));
  TEST_ASSERT_TRUE(unitVccIsLow(true, 3000, 4000));
  // At or above the floor -> not low.
  TEST_ASSERT_FALSE(unitVccIsLow(true, 4000, 4000));
  TEST_ASSERT_FALSE(unitVccIsLow(true, 4800, UNIT_VCC_MIN_FLOOR_MV));
  // vccMin==0 is the "no reading" sentinel, never an alert even if < floor.
  TEST_ASSERT_FALSE(unitVccIsLow(true, 0, UNIT_VCC_MIN_FLOOR_MV));
  // Invalid vitals never alert regardless of the value.
  TEST_ASSERT_FALSE(unitVccIsLow(false, 3000, UNIT_VCC_MIN_FLOOR_MV));
}

// --- reset-cause decode (#368) -----------------------------------------------

static void test_reset_cause_priority_brownout_over_watchdog() {
  // BORF (bit2) + WDRF (bit3) both set -> brownout wins (most actionable).
  TEST_ASSERT_EQUAL(RESET_BROWNOUT, unitResetCauseDecode((1<<2) | (1<<3)));
}

static void test_reset_cause_each_flag() {
  TEST_ASSERT_EQUAL(RESET_WATCHDOG, unitResetCauseDecode(1<<3));
  TEST_ASSERT_EQUAL(RESET_EXTERNAL, unitResetCauseDecode(1<<1));
  TEST_ASSERT_EQUAL(RESET_POWER_ON, unitResetCauseDecode(1<<0));
  TEST_ASSERT_EQUAL(RESET_UNKNOWN,  unitResetCauseDecode(0));
}

static void test_reset_cause_name_nonnull() {
  TEST_ASSERT_EQUAL_STRING("brownout", unitResetCauseName(RESET_BROWNOUT));
  TEST_ASSERT_EQUAL_STRING("power-on", unitResetCauseName(RESET_POWER_ON));
}

// --- reboot-detect edge helper (#368) ----------------------------------------

static void test_reboot_detect_primes_silent() {
  UnitRebootWatch w{};
  // First observation only primes — never logs a phantom reboot.
  TEST_ASSERT_FALSE(unitRebootDetect(w, 100, 0, 0));
}

static void test_reboot_detect_uptime_drop() {
  UnitRebootWatch w{};
  unitRebootDetect(w, 500, 0, 0);           // prime
  TEST_ASSERT_TRUE(unitRebootDetect(w, 12, 0, 0));   // uptime fell -> reboot
  TEST_ASSERT_FALSE(unitRebootDetect(w, 30, 0, 0));  // climbing -> no event
}

static void test_reboot_detect_counter_climb() {
  UnitRebootWatch w{};
  unitRebootDetect(w, 500, 1, 0);           // prime
  // Fast reboot: uptime may not have visibly dropped but brownout count rose.
  TEST_ASSERT_TRUE(unitRebootDetect(w, 505, 2, 0));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_first_seen_fault_is_an_onset);
  RUN_TEST(test_unchanged_state_is_silent);
  RUN_TEST(test_new_condition_onsets_without_disturbing_existing);
  RUN_TEST(test_recoverable_condition_clearing_logs_recovery);
  RUN_TEST(test_nonrecoverable_condition_clears_silently);
  RUN_TEST(test_unreadable_unit_does_not_fake_recovery);
  RUN_TEST(test_going_stale_while_home_failed_logs_only_stale);
  RUN_TEST(test_mismatch_diag_gap_carries_not_reonsets);
  RUN_TEST(test_recoverable_mask_membership);
  RUN_TEST(test_low_vcc_onsets_once_and_stays_latched);
  RUN_TEST(test_low_vcc_vitals_gap_carries_no_recovery);
  RUN_TEST(test_unit_vcc_is_low_threshold);
  RUN_TEST(test_reset_cause_priority_brownout_over_watchdog);
  RUN_TEST(test_reset_cause_each_flag);
  RUN_TEST(test_reset_cause_name_nonnull);
  RUN_TEST(test_reboot_detect_primes_silent);
  RUN_TEST(test_reboot_detect_uptime_drop);
  RUN_TEST(test_reboot_detect_counter_climb);
  return UNITY_END();
}
