// Host-side unit tests for the pure drift-detection logic in UnitDrift.h
// (#263/#264) and the self-test wire encode in UnitSelfTest.h (#265):
// position tracking mod one revolution, hall-edge deviation folding,
// event/threshold policy, physical-letter estimation and both I2C reply
// encodings. The hall/stepper/ISR glue in the .ino files is bench tier.

#include <unity.h>
#include <stdint.h>
#include "../../UnitDrift.h"
#include "UnitSelfTest.h"

// The unit's real constants (Unit.ino STEPS / AMOUNTFLAPS). Tests pass them
// explicitly — the headers stay pure and never include sketch globals.
static const uint16_t kStepsPerRev = 2038;
static const uint8_t kFlaps = 45;

void setUp() {}
void tearDown() {}

static DriftState freshState() {
  DriftState s;
  driftReset(s);
  return s;
}

// --- driftAdvance: position tracking mod one revolution ---------------------

static void test_advance_accumulates_forward_steps() {
  DriftState s = freshState();
  driftAdvance(s, 100, kStepsPerRev);
  driftAdvance(s, 45, kStepsPerRev);
  TEST_ASSERT_EQUAL_UINT16(145, s.drumPosition);
}

static void test_advance_wraps_at_one_revolution() {
  DriftState s = freshState();
  driftAdvance(s, 2000, kStepsPerRev);
  driftAdvance(s, 50, kStepsPerRev);
  TEST_ASSERT_EQUAL_UINT16(12, s.drumPosition);
}

static void test_advance_negative_steps_wrap_backward() {
  // Jog backwards must keep the estimate coherent.
  DriftState s = freshState();
  driftAdvance(s, 10, kStepsPerRev);
  driftAdvance(s, -30, kStepsPerRev);
  TEST_ASSERT_EQUAL_UINT16(2018, s.drumPosition);
}

static void test_advance_spanning_multiple_revs() {
  DriftState s = freshState();
  driftAdvance(s, 3 * 2038 + 7, kStepsPerRev);
  TEST_ASSERT_EQUAL_UINT16(7, s.drumPosition);
}

// --- driftFoldDeviation: edge deviation folded to signed half-rev ------------

static void test_fold_small_positive_stays_positive() {
  // Edge fired with the counter 8 past it: belief ran ahead of the drum
  // (missed steps).
  TEST_ASSERT_EQUAL_INT16(8, driftFoldDeviation(8, kStepsPerRev));
}

static void test_fold_near_rev_becomes_small_negative() {
  // Edge fired 8 steps before the counter expected it: drum ran ahead of
  // belief (external nudge).
  TEST_ASSERT_EQUAL_INT16(-8, driftFoldDeviation(2030, kStepsPerRev));
}

static void test_fold_zero_is_zero() {
  TEST_ASSERT_EQUAL_INT16(0, driftFoldDeviation(0, kStepsPerRev));
}

static void test_fold_half_rev_boundary() {
  TEST_ASSERT_EQUAL_INT16(1019, driftFoldDeviation(1019, kStepsPerRev));
  TEST_ASSERT_EQUAL_INT16(-1018, driftFoldDeviation(1020, kStepsPerRev));
}

// --- driftObserveEdge: event decision + resync -------------------------------

static void test_observe_edge_in_place_is_no_event() {
  DriftState s = freshState();
  driftMarkSynced(s);  // calibrate found the marker once already
  driftAdvance(s, kStepsPerRev, kStepsPerRev);  // exactly one revolution
  TEST_ASSERT_FALSE(driftObserveEdge(s, kStepsPerRev, DRIFT_THRESHOLD_STEPS));
  TEST_ASSERT_EQUAL_UINT8(0, s.driftEvents);
  TEST_ASSERT_EQUAL_UINT16(0, s.drumPosition);  // resynced to the edge
  TEST_ASSERT_TRUE(s.positionKnown);
}

static void test_observe_edge_within_threshold_is_no_event() {
  DriftState s = freshState();
  driftMarkSynced(s);
  driftAdvance(s, kStepsPerRev + DRIFT_THRESHOLD_STEPS, kStepsPerRev);
  TEST_ASSERT_FALSE(driftObserveEdge(s, kStepsPerRev, DRIFT_THRESHOLD_STEPS));
  TEST_ASSERT_EQUAL_UINT16(0, s.drumPosition);
}

static void test_observe_edge_past_threshold_counts_event() {
  DriftState s = freshState();
  driftMarkSynced(s);
  driftAdvance(s, kStepsPerRev + DRIFT_THRESHOLD_STEPS + 1, kStepsPerRev);
  TEST_ASSERT_TRUE(driftObserveEdge(s, kStepsPerRev, DRIFT_THRESHOLD_STEPS));
  TEST_ASSERT_EQUAL_UINT8(1, s.driftEvents);
  TEST_ASSERT_EQUAL_INT16(DRIFT_THRESHOLD_STEPS + 1, s.lastDriftSteps);
  TEST_ASSERT_EQUAL_UINT16(0, s.drumPosition);  // resynced regardless
}

static void test_observe_edge_negative_drift_counts_event() {
  DriftState s = freshState();
  driftMarkSynced(s);
  // Drum ahead of belief: edge arrives 40 steps early.
  driftAdvance(s, kStepsPerRev - 40, kStepsPerRev);
  TEST_ASSERT_TRUE(driftObserveEdge(s, kStepsPerRev, DRIFT_THRESHOLD_STEPS));
  TEST_ASSERT_EQUAL_INT16(-40, s.lastDriftSteps);
}

static void test_observe_edge_before_first_sync_never_counts() {
  // Until calibrate has synced once, there is no expectation to deviate
  // from — the first boot homing must not log a phantom drift event.
  DriftState s = freshState();
  driftAdvance(s, 500, kStepsPerRev);
  TEST_ASSERT_FALSE(driftObserveEdge(s, kStepsPerRev, DRIFT_THRESHOLD_STEPS));
  TEST_ASSERT_EQUAL_UINT8(0, s.driftEvents);
  TEST_ASSERT_TRUE(s.positionKnown);  // but the edge still syncs
  TEST_ASSERT_EQUAL_UINT16(0, s.drumPosition);
}

static void test_observe_edge_event_counter_saturates() {
  DriftState s = freshState();
  driftMarkSynced(s);
  s.driftEvents = 0xFF;
  driftAdvance(s, kStepsPerRev + 100, kStepsPerRev);
  TEST_ASSERT_TRUE(driftObserveEdge(s, kStepsPerRev, DRIFT_THRESHOLD_STEPS));
  TEST_ASSERT_EQUAL_UINT8(0xFF, s.driftEvents);
}

// --- driftPhysicalLetter: hall-corrected letter estimate ----------------------

static void test_physical_letter_unknown_before_sync() {
  DriftState s = freshState();
  TEST_ASSERT_EQUAL_UINT8(DRIFT_LETTER_UNKNOWN,
                          driftPhysicalLetter(s, 0, kStepsPerRev, kFlaps));
}

static void test_physical_letter_at_offset_is_blank() {
  // calibrate parks at edge + calOffset and calls that letter 0.
  DriftState s = freshState();
  driftMarkSynced(s);
  driftAdvance(s, 120, kStepsPerRev);
  TEST_ASSERT_EQUAL_UINT8(0, driftPhysicalLetter(s, 120, kStepsPerRev, kFlaps));
}

static void test_physical_letter_tracks_flap_increments() {
  DriftState s = freshState();
  driftMarkSynced(s);
  int16_t calOffset = 120;
  driftAdvance(s, calOffset, kStepsPerRev);
  // Step letter-by-letter the way stepFlaps does (whole + carried fraction)
  // and require the estimate to match at every one of the 45 positions.
  float missed = 0;
  for (uint8_t letter = 1; letter < kFlaps; letter++) {
    int whole = kStepsPerRev / kFlaps;
    missed += (float)kStepsPerRev / kFlaps - whole;
    if (missed > 1) {
      whole++;
      missed--;
    }
    driftAdvance(s, whole, kStepsPerRev);
    TEST_ASSERT_EQUAL_UINT8(
        letter, driftPhysicalLetter(s, calOffset, kStepsPerRev, kFlaps));
  }
}

static void test_physical_letter_with_negative_offset() {
  DriftState s = freshState();
  driftMarkSynced(s);
  int16_t calOffset = -50;
  // Park at letter 0 = edge - 50 steps = position 1988.
  driftAdvance(s, kStepsPerRev - 50, kStepsPerRev);
  TEST_ASSERT_EQUAL_UINT8(0,
                          driftPhysicalLetter(s, calOffset, kStepsPerRev, kFlaps));
}

static void test_physical_letter_survives_one_step_rounding_error() {
  // The belief accumulator can sit a step or two off the exact letter
  // position — nearest-letter rounding must not flip to a neighbor.
  DriftState s = freshState();
  driftMarkSynced(s);
  uint32_t letter10 = (10UL * kStepsPerRev + kFlaps / 2) / kFlaps;
  driftAdvance(s, (int)letter10 - 2, kStepsPerRev);
  TEST_ASSERT_EQUAL_UINT8(10, driftPhysicalLetter(s, 0, kStepsPerRev, kFlaps));
  driftAdvance(s, 4, kStepsPerRev);
  TEST_ASSERT_EQUAL_UINT8(10, driftPhysicalLetter(s, 0, kStepsPerRev, kFlaps));
}

// --- driftEncodeDiagReply: I2C wire format (6 bytes, masked XOR) --------------

static void test_encode_diag_layout_and_checksum() {
  uint8_t buf[DRIFT_REPLY_LEN];
  driftEncodeDiagReply(7, DRIFT_FLAG_PENDING | DRIFT_FLAG_POSITION_KNOWN, 3,
                       -30, buf);
  TEST_ASSERT_EQUAL_UINT8(7, buf[0]);
  TEST_ASSERT_EQUAL_UINT8(0x03, buf[1]);
  TEST_ASSERT_EQUAL_UINT8(3, buf[2]);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)(int8_t)-30, buf[3]);
  TEST_ASSERT_EQUAL_UINT8(0, buf[4]);
  uint8_t expected =
      (uint8_t)((buf[0] ^ buf[1] ^ buf[2] ^ buf[3] ^ buf[4]) ^
                DRIFT_REPLY_CHECKSUM_MASK);
  TEST_ASSERT_EQUAL_UINT8(expected, buf[5]);
}

static void test_encode_diag_clamps_large_drift_magnitude() {
  uint8_t buf[DRIFT_REPLY_LEN];
  driftEncodeDiagReply(0, 0, 0, 1000, buf);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)(int8_t)127, buf[3]);
  driftEncodeDiagReply(0, 0, 0, -1000, buf);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)(int8_t)-127, buf[3]);
}

static void test_encode_diag_all_zero_checksum_is_mask() {
  // An old unit answers the unknown opcode with 0x00/0xFF bus padding —
  // the mask keeps all-zero garbage from validating (#106 class).
  uint8_t buf[DRIFT_REPLY_LEN];
  driftEncodeDiagReply(0, 0, 0, 0, buf);
  TEST_ASSERT_EQUAL_UINT8(DRIFT_REPLY_CHECKSUM_MASK, buf[5]);
}

// --- selfTestEncodeReply: I2C wire format (9 bytes, masked XOR) ---------------

static void test_encode_selftest_layout_and_checksum() {
  SelfTestResult r;
  r.state = SELFTEST_STATE_OK;
  r.stepsPerRev = 2041;      // 0x07F9
  r.hallWindowSteps = 90;    // 0x005A
  r.revTimeMs = 6120;        // 0x17E8
  uint8_t buf[SELFTEST_REPLY_LEN];
  selfTestEncodeReply(r, buf);
  TEST_ASSERT_EQUAL_UINT8(SELFTEST_STATE_OK, buf[0]);
  TEST_ASSERT_EQUAL_UINT8(0xF9, buf[1]);
  TEST_ASSERT_EQUAL_UINT8(0x07, buf[2]);
  TEST_ASSERT_EQUAL_UINT8(0x5A, buf[3]);
  TEST_ASSERT_EQUAL_UINT8(0x00, buf[4]);
  TEST_ASSERT_EQUAL_UINT8(0xE8, buf[5]);
  TEST_ASSERT_EQUAL_UINT8(0x17, buf[6]);
  TEST_ASSERT_EQUAL_UINT8(0x00, buf[7]);
  uint8_t x = 0;
  for (int i = 0; i < SELFTEST_REPLY_LEN - 1; i++) x ^= buf[i];
  TEST_ASSERT_EQUAL_UINT8((uint8_t)(x ^ SELFTEST_REPLY_CHECKSUM_MASK), buf[8]);
}

// --- self-test failure reason (#404) -----------------------------------------
// The three modes used to collapse into a bare "failed" with every
// measurement zeroed, so the network could not tell a magnet against the
// sensor from a magnet that fell off from a drum that binds.

static void test_encode_selftest_reason_rides_the_reserved_byte() {
  // Byte 7 was reserved, so carrying the reason costs no reply length.
  SelfTestResult r;
  r.state = SELFTEST_STATE_FAILED;
  r.stepsPerRev = 0;
  r.hallWindowSteps = 46;  // phase 2 knew this before it gave up
  r.revTimeMs = 0;
  r.reason = SELFTEST_REASON_REV_INCOMPLETE;
  uint8_t buf[SELFTEST_REPLY_LEN];
  selfTestEncodeReply(r, buf);
  TEST_ASSERT_EQUAL_UINT8(SELFTEST_STATE_FAILED, buf[0]);
  TEST_ASSERT_EQUAL_UINT8(46, buf[3]);
  TEST_ASSERT_EQUAL_UINT8(SELFTEST_REASON_REV_INCOMPLETE, buf[7]);
  uint8_t x = 0;
  for (int i = 0; i < SELFTEST_REPLY_LEN - 1; i++) x ^= buf[i];
  TEST_ASSERT_EQUAL_UINT8((uint8_t)(x ^ SELFTEST_REPLY_CHECKSUM_MASK), buf[8]);
}

static void test_selftest_reason_is_inside_the_checksum() {
  // A corrupted reason must be REJECTED, not acted on — it is a repair
  // instruction for a person standing at the wall.
  SelfTestResult r;
  r.state = SELFTEST_STATE_FAILED;
  r.stepsPerRev = 0;
  r.hallWindowSteps = 0;
  r.revTimeMs = 0;
  r.reason = SELFTEST_REASON_HALL_STUCK;
  uint8_t a[SELFTEST_REPLY_LEN];
  selfTestEncodeReply(r, a);
  r.reason = SELFTEST_REASON_HALL_NEVER;
  uint8_t b[SELFTEST_REPLY_LEN];
  selfTestEncodeReply(r, b);
  TEST_ASSERT_NOT_EQUAL(a[SELFTEST_REPLY_LEN - 1], b[SELFTEST_REPLY_LEN - 1]);
}

static void test_selftest_reason_names_are_distinct_and_actionable() {
  TEST_ASSERT_EQUAL_STRING("none", selfTestReasonName(SELFTEST_REASON_NONE));
  TEST_ASSERT_EQUAL_STRING("hall-stuck",
                           selfTestReasonName(SELFTEST_REASON_HALL_STUCK));
  TEST_ASSERT_EQUAL_STRING("hall-never",
                           selfTestReasonName(SELFTEST_REASON_HALL_NEVER));
  TEST_ASSERT_EQUAL_STRING("rev-incomplete",
                           selfTestReasonName(SELFTEST_REASON_REV_INCOMPLETE));
  // An out-of-vocabulary code must not print as a real diagnosis.
  TEST_ASSERT_EQUAL_STRING("none", selfTestReasonName(99));
}

static void test_encode_selftest_never_run_checksum_is_mask() {
  SelfTestResult r;  // zero-initialised: never run
  r.state = SELFTEST_STATE_NEVER;
  r.stepsPerRev = 0;
  r.hallWindowSteps = 0;
  r.revTimeMs = 0;
  uint8_t buf[SELFTEST_REPLY_LEN];
  selfTestEncodeReply(r, buf);
  TEST_ASSERT_EQUAL_UINT8(SELFTEST_REPLY_CHECKSUM_MASK, buf[8]);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_advance_accumulates_forward_steps);
  RUN_TEST(test_advance_wraps_at_one_revolution);
  RUN_TEST(test_advance_negative_steps_wrap_backward);
  RUN_TEST(test_advance_spanning_multiple_revs);
  RUN_TEST(test_fold_small_positive_stays_positive);
  RUN_TEST(test_fold_near_rev_becomes_small_negative);
  RUN_TEST(test_fold_zero_is_zero);
  RUN_TEST(test_fold_half_rev_boundary);
  RUN_TEST(test_observe_edge_in_place_is_no_event);
  RUN_TEST(test_observe_edge_within_threshold_is_no_event);
  RUN_TEST(test_observe_edge_past_threshold_counts_event);
  RUN_TEST(test_observe_edge_negative_drift_counts_event);
  RUN_TEST(test_observe_edge_before_first_sync_never_counts);
  RUN_TEST(test_observe_edge_event_counter_saturates);
  RUN_TEST(test_physical_letter_unknown_before_sync);
  RUN_TEST(test_physical_letter_at_offset_is_blank);
  RUN_TEST(test_physical_letter_tracks_flap_increments);
  RUN_TEST(test_physical_letter_with_negative_offset);
  RUN_TEST(test_physical_letter_survives_one_step_rounding_error);
  RUN_TEST(test_encode_diag_layout_and_checksum);
  RUN_TEST(test_encode_diag_clamps_large_drift_magnitude);
  RUN_TEST(test_encode_diag_all_zero_checksum_is_mask);
  RUN_TEST(test_encode_selftest_layout_and_checksum);
  RUN_TEST(test_encode_selftest_reason_rides_the_reserved_byte);
  RUN_TEST(test_selftest_reason_is_inside_the_checksum);
  RUN_TEST(test_selftest_reason_names_are_distinct_and_actionable);
  RUN_TEST(test_encode_selftest_never_run_checksum_is_mask);
  UNITY_END();
  return 0;
}
