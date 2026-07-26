// Host-side unit tests for RescueSlotStatus.h (#391) — the factory
// rescue-slot verdict Master reports without booting into rescue.
//
// The slot's own app descriptor cannot identify the image (pioarduino
// freezes the build stamp at framework-assembly time, #200), so the rev
// comes from an NVS record written at install time and is only trusted
// while its sha256 still matches the partition's actual content.

#include <ArduinoFake.h>
#include <unity.h>

#include "../../RescueSlotStatus.h"

void setUp() {}
void tearDown() {}

static SlotRecord recordFor(const char* rev) {
  SlotRecord r;
  r.ok = true;
  r.seq = 0;
  snprintf(r.rev, sizeof(r.rev), "%s", rev);
  return r;
}

static SlotRecord noRecord() { return SlotRecord(); }

// --- absent / empty slot ------------------------------------------------------

static void test_absent_partition_reports_absent() {
  RescueSlotFacts f =
      rescueSlotFacts(false, false, noRecord(), false, "f14a7b6");
  TEST_ASSERT_FALSE(f.present);
  TEST_ASSERT_FALSE(f.valid);
  TEST_ASSERT_FALSE(f.identified);
  TEST_ASSERT_FALSE(f.stale);
  TEST_ASSERT_EQUAL_STRING("", f.rev);
  TEST_ASSERT_TRUE(f.warn);
  TEST_ASSERT_EQUAL(RESCUE_SLOT_ABSENT, f.state);
}

static void test_present_but_no_valid_image_reports_empty() {
  RescueSlotFacts f =
      rescueSlotFacts(true, false, noRecord(), false, "f14a7b6");
  TEST_ASSERT_TRUE(f.present);
  TEST_ASSERT_FALSE(f.valid);
  TEST_ASSERT_TRUE(f.warn);
  TEST_ASSERT_EQUAL(RESCUE_SLOT_EMPTY, f.state);
}

// A record left over from a previous image must never make an EMPTY slot
// look identified — the image is what matters, not the bookkeeping.
static void test_invalid_image_never_inherits_a_matching_record() {
  RescueSlotFacts f =
      rescueSlotFacts(true, false, recordFor("b0e3fe6"), true, "f14a7b6");
  TEST_ASSERT_FALSE(f.identified);
  TEST_ASSERT_EQUAL_STRING("", f.rev);
  TEST_ASSERT_EQUAL(RESCUE_SLOT_EMPTY, f.state);
}

// --- identified slot ----------------------------------------------------------

static void test_matching_record_at_running_rev_is_ok() {
  RescueSlotFacts f =
      rescueSlotFacts(true, true, recordFor("f14a7b6"), true, "f14a7b6");
  TEST_ASSERT_TRUE(f.valid);
  TEST_ASSERT_TRUE(f.identified);
  TEST_ASSERT_FALSE(f.stale);
  TEST_ASSERT_FALSE(f.warn);
  TEST_ASSERT_EQUAL_STRING("f14a7b6", f.rev);
  TEST_ASSERT_EQUAL(RESCUE_SLOT_OK, f.state);
}

// The #391 case observed on .20: a Jul-13 rescue image under a Jul-25 app.
static void test_matching_record_behind_running_rev_is_stale() {
  RescueSlotFacts f =
      rescueSlotFacts(true, true, recordFor("b0e3fe6"), true, "f14a7b6");
  TEST_ASSERT_TRUE(f.identified);
  TEST_ASSERT_TRUE(f.stale);
  TEST_ASSERT_TRUE(f.warn);
  TEST_ASSERT_EQUAL_STRING("b0e3fe6", f.rev);
  TEST_ASSERT_EQUAL(RESCUE_SLOT_STALE, f.state);
}

// --- unidentified slot --------------------------------------------------------

// Installed out of band (esptool, or a build predating the record): the
// image is bootable but we cannot say what it is. Never guess a rev.
static void test_no_record_is_unidentified_not_stale() {
  RescueSlotFacts f = rescueSlotFacts(true, true, noRecord(), false, "f14a7b6");
  TEST_ASSERT_TRUE(f.valid);
  TEST_ASSERT_FALSE(f.identified);
  TEST_ASSERT_FALSE(f.stale);
  TEST_ASSERT_TRUE(f.warn);
  TEST_ASSERT_EQUAL_STRING("", f.rev);
  TEST_ASSERT_EQUAL(RESCUE_SLOT_UNIDENTIFIED, f.state);
}

// Slot rewritten behind the record's back — the sha guard must demote it
// rather than report the stale record's rev as truth.
static void test_sha_mismatch_demotes_to_unidentified() {
  RescueSlotFacts f =
      rescueSlotFacts(true, true, recordFor("b0e3fe6"), false, "f14a7b6");
  TEST_ASSERT_FALSE(f.identified);
  TEST_ASSERT_EQUAL_STRING("", f.rev);
  TEST_ASSERT_EQUAL(RESCUE_SLOT_UNIDENTIFIED, f.state);
}

// --- running-rev edge cases ---------------------------------------------------

// Without a running rev to compare against, staleness is unknowable —
// report the rev but never claim it is behind.
static void test_missing_running_rev_reports_rev_without_staleness() {
  RescueSlotFacts f = rescueSlotFacts(true, true, recordFor("b0e3fe6"), true, "");
  TEST_ASSERT_TRUE(f.identified);
  TEST_ASSERT_FALSE(f.stale);
  TEST_ASSERT_EQUAL_STRING("b0e3fe6", f.rev);
  TEST_ASSERT_EQUAL(RESCUE_SLOT_OK, f.state);
}

static void test_null_running_rev_is_safe() {
  RescueSlotFacts f =
      rescueSlotFacts(true, true, recordFor("b0e3fe6"), true, nullptr);
  TEST_ASSERT_TRUE(f.identified);
  TEST_ASSERT_FALSE(f.stale);
  TEST_ASSERT_EQUAL_STRING("b0e3fe6", f.rev);
}

// A record whose rev field is empty carries no identity even though the
// record itself parsed — treat it as unidentified, not as rev "".
static void test_record_with_empty_rev_is_unidentified() {
  RescueSlotFacts f = rescueSlotFacts(true, true, recordFor(""), true, "f14a7b6");
  TEST_ASSERT_FALSE(f.identified);
  TEST_ASSERT_EQUAL(RESCUE_SLOT_UNIDENTIFIED, f.state);
}

// Round-trip through the REAL formatter, not a hand-built record. The
// hand-built variants above cannot reach this: formatSlotRecord guarantees
// a non-empty rev by substituting "?" for one that filters to nothing, so
// an identity-less record actually arrives here as rev "?" — which must
// read UNIDENTIFIED, never STALE against the running rev.
static void test_formatter_placeholder_rev_round_trips_as_unidentified() {
  static const uint8_t sha[32] = {0};
  char buf[SLOT_RECORD_BUF_LEN];
  TEST_ASSERT_TRUE(formatSlotRecord(buf, sizeof(buf), 0, sha, ""));
  SlotRecord parsed = parseSlotRecord(buf);
  TEST_ASSERT_TRUE(parsed.ok);
  TEST_ASSERT_EQUAL_STRING("?", parsed.rev);  // pins the formatter's behaviour

  RescueSlotFacts f = rescueSlotFacts(true, true, parsed, true, "f14a7b6");
  TEST_ASSERT_FALSE(f.identified);
  TEST_ASSERT_FALSE(f.stale);
  TEST_ASSERT_EQUAL_STRING("", f.rev);
  TEST_ASSERT_EQUAL(RESCUE_SLOT_UNIDENTIFIED, f.state);
}

// A rev made entirely of characters the formatter drops collapses to the
// same placeholder — same verdict, via a different route.
static void test_formatter_all_filtered_rev_is_unidentified() {
  static const uint8_t sha[32] = {0};
  char buf[SLOT_RECORD_BUF_LEN];
  TEST_ASSERT_TRUE(formatSlotRecord(buf, sizeof(buf), 0, sha, "|||\\\"|||"));
  RescueSlotFacts f =
      rescueSlotFacts(true, true, parseSlotRecord(buf), true, "f14a7b6");
  TEST_ASSERT_EQUAL(RESCUE_SLOT_UNIDENTIFIED, f.state);
}

// A real rev must still survive the same round trip — the guard above must
// not swallow legitimate identities.
static void test_formatter_real_rev_round_trips_as_identified() {
  static const uint8_t sha[32] = {0};
  char buf[SLOT_RECORD_BUF_LEN];
  TEST_ASSERT_TRUE(formatSlotRecord(buf, sizeof(buf), 0, sha, "b0e3fe6"));
  RescueSlotFacts f =
      rescueSlotFacts(true, true, parseSlotRecord(buf), true, "f14a7b6");
  TEST_ASSERT_TRUE(f.identified);
  TEST_ASSERT_TRUE(f.stale);
  TEST_ASSERT_EQUAL_STRING("b0e3fe6", f.rev);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_absent_partition_reports_absent);
  RUN_TEST(test_present_but_no_valid_image_reports_empty);
  RUN_TEST(test_invalid_image_never_inherits_a_matching_record);
  RUN_TEST(test_matching_record_at_running_rev_is_ok);
  RUN_TEST(test_matching_record_behind_running_rev_is_stale);
  RUN_TEST(test_no_record_is_unidentified_not_stale);
  RUN_TEST(test_sha_mismatch_demotes_to_unidentified);
  RUN_TEST(test_missing_running_rev_reports_rev_without_staleness);
  RUN_TEST(test_null_running_rev_is_safe);
  RUN_TEST(test_record_with_empty_rev_is_unidentified);
  RUN_TEST(test_formatter_placeholder_rev_round_trips_as_unidentified);
  RUN_TEST(test_formatter_all_filtered_rev_is_unidentified);
  RUN_TEST(test_formatter_real_rev_round_trips_as_identified);
  return UNITY_END();
}
