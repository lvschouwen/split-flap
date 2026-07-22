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
  // Guard the policy: exactly home-failed + stale recover; hall-never + mismatch
  // do not.
  TEST_ASSERT_TRUE(UNIT_EVT_RECOVERABLE & UNIT_EVT_HOME_FAILED);
  TEST_ASSERT_TRUE(UNIT_EVT_RECOVERABLE & UNIT_EVT_STALE);
  TEST_ASSERT_FALSE(UNIT_EVT_RECOVERABLE & UNIT_EVT_HALL_NEVER);
  TEST_ASSERT_FALSE(UNIT_EVT_RECOVERABLE & UNIT_EVT_MISMATCH);
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
  return UNITY_END();
}
