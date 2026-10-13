// Host-side unit tests for RescueSlotStatus.h (#391, #613) — the factory
// rescue-slot verdict Master reports without booting into rescue — and for
// the marker the rescue image carries (../release/RescueSourceHead.h).
//
// The rev comes from an NVS record written at install time, trusted only
// while its sha256 still matches the partition; the vintage from the rescue
// source head the image embeds, against the one this build knows.

#include <ArduinoFake.h>
#include <unity.h>

#include "../../RescueSlotStatus.h"

void setUp() {}
void tearDown() {}

#define OWN "f14a7b6"

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
  RescueSlotFacts f = rescueSlotFacts(false, false, noRecord(), false, OWN, OWN);
  TEST_ASSERT_EQUAL(RESCUE_SLOT_ABSENT, f.state);
  TEST_ASSERT_FALSE(f.present);
  TEST_ASSERT_TRUE(f.warn);
  TEST_ASSERT_EQUAL_STRING("", f.rev);
}

static void test_present_but_no_valid_image_reports_empty() {
  RescueSlotFacts f = rescueSlotFacts(true, false, noRecord(), false, OWN, OWN);
  TEST_ASSERT_EQUAL(RESCUE_SLOT_EMPTY, f.state);
  TEST_ASSERT_TRUE(f.warn);
  TEST_ASSERT_EQUAL_STRING("install the current rescue", f.todo);
}

static void test_invalid_image_never_inherits_a_matching_record_or_marker() {
  RescueSlotFacts f = rescueSlotFacts(true, false, recordFor("b0e3fe6"), true, OWN, OWN);
  TEST_ASSERT_EQUAL(RESCUE_SLOT_EMPTY, f.state);
  TEST_ASSERT_FALSE(f.identified);
  TEST_ASSERT_EQUAL_STRING("", f.rev);
  TEST_ASSERT_EQUAL_STRING("", f.sourceHead);
}

// --- the verdict is the source head's ---------------------------------------

static void test_the_same_source_head_is_ok_whatever_the_revs() {
  RescueSlotFacts f = rescueSlotFacts(true, true, recordFor("b0e3fe6"), true, OWN, OWN);
  TEST_ASSERT_EQUAL(RESCUE_SLOT_OK, f.state);
  TEST_ASSERT_TRUE(f.identified);
  TEST_ASSERT_FALSE(f.outdated);
  TEST_ASSERT_FALSE(f.warn);
  TEST_ASSERT_EQUAL_STRING("b0e3fe6", f.rev);
  TEST_ASSERT_EQUAL_STRING(OWN, f.sourceHead);
  TEST_ASSERT_EQUAL_STRING("", f.todo);
}

static void test_another_source_head_is_outdated_but_not_a_warning() {
  RescueSlotFacts f = rescueSlotFacts(true, true, recordFor(OWN), true, "0123abc", OWN);
  TEST_ASSERT_EQUAL(RESCUE_SLOT_OUTDATED, f.state);
  TEST_ASSERT_TRUE(f.outdated);
  TEST_ASSERT_FALSE(f.warn);
  TEST_ASSERT_EQUAL_STRING("0123abc", f.sourceHead);
  TEST_ASSERT_EQUAL_STRING("install the current rescue", f.todo);
}

static void test_a_marker_identifies_the_image_without_a_record() {
  RescueSlotFacts f = rescueSlotFacts(true, true, noRecord(), false, OWN, OWN);
  TEST_ASSERT_EQUAL(RESCUE_SLOT_OK, f.state);
  TEST_ASSERT_FALSE(f.identified);
  TEST_ASSERT_EQUAL_STRING("", f.rev);
  TEST_ASSERT_FALSE(f.warn);
}

static void test_a_recorded_image_without_a_marker_is_of_unknown_vintage() {
  RescueSlotFacts f = rescueSlotFacts(true, true, recordFor("b0e3fe6"), true, "", OWN);
  TEST_ASSERT_EQUAL(RESCUE_SLOT_UNKNOWN_VINTAGE, f.state);
  TEST_ASSERT_TRUE(f.identified);
  TEST_ASSERT_FALSE(f.outdated);
  TEST_ASSERT_FALSE(f.warn);
  TEST_ASSERT_EQUAL_STRING("install the current rescue to be sure", f.todo);
  RescueSlotFacts g = rescueSlotFacts(true, true, recordFor("b0e3fe6"), true, nullptr, OWN);
  TEST_ASSERT_EQUAL(RESCUE_SLOT_UNKNOWN_VINTAGE, g.state);
}

static void test_nothing_known_is_unidentified_and_a_warning() {
  RescueSlotFacts f = rescueSlotFacts(true, true, noRecord(), false, "", OWN);
  TEST_ASSERT_EQUAL(RESCUE_SLOT_UNIDENTIFIED, f.state);
  TEST_ASSERT_TRUE(f.warn);
  TEST_ASSERT_EQUAL_STRING("", f.rev);
}

static void test_sha_mismatch_drops_the_rev_not_the_marker() {
  RescueSlotFacts f = rescueSlotFacts(true, true, recordFor("b0e3fe6"), false, OWN, OWN);
  TEST_ASSERT_EQUAL(RESCUE_SLOT_OK, f.state);
  TEST_ASSERT_FALSE(f.identified);
  TEST_ASSERT_EQUAL_STRING("", f.rev);
  RescueSlotFacts g = rescueSlotFacts(true, true, recordFor("b0e3fe6"), false, "", OWN);
  TEST_ASSERT_EQUAL(RESCUE_SLOT_UNIDENTIFIED, g.state);
}

static void test_a_build_without_its_own_head_cannot_call_a_slot_current() {
  RescueSlotFacts f = rescueSlotFacts(true, true, recordFor(OWN), true, OWN, "");
  TEST_ASSERT_EQUAL(RESCUE_SLOT_OUTDATED, f.state);
  RescueSlotFacts g = rescueSlotFacts(true, true, recordFor(OWN), true, OWN, nullptr);
  TEST_ASSERT_EQUAL(RESCUE_SLOT_OUTDATED, g.state);
}

static void test_only_recovery_blocking_states_warn() {
  TEST_ASSERT_TRUE(rescueSlotFacts(false, false, noRecord(), false, OWN, OWN).warn);
  TEST_ASSERT_TRUE(rescueSlotFacts(true, false, noRecord(), false, OWN, OWN).warn);
  TEST_ASSERT_TRUE(rescueSlotFacts(true, true, noRecord(), false, "", OWN).warn);
  TEST_ASSERT_FALSE(rescueSlotFacts(true, true, recordFor("b0e3fe6"), true, "", OWN).warn);
  TEST_ASSERT_FALSE(rescueSlotFacts(true, true, noRecord(), false, "0123abc", OWN).warn);
}

// --- the record's placeholder shapes -----------------------------------------

static void test_record_with_empty_rev_is_not_identified() {
  RescueSlotFacts f = rescueSlotFacts(true, true, recordFor(""), true, "", OWN);
  TEST_ASSERT_EQUAL(RESCUE_SLOT_UNIDENTIFIED, f.state);
  TEST_ASSERT_FALSE(f.identified);
}

static void test_formatter_placeholder_rev_round_trips_as_unidentified() {
  char buf[SLOT_RECORD_BUF_LEN];
  uint8_t sha[32] = {0};
  TEST_ASSERT_TRUE(formatSlotRecord(buf, sizeof(buf), 0, sha, ""));
  SlotRecord parsed = parseSlotRecord(buf);
  TEST_ASSERT_TRUE(parsed.ok);
  TEST_ASSERT_EQUAL_STRING("?", parsed.rev);
  RescueSlotFacts f = rescueSlotFacts(true, true, parsed, true, "", OWN);
  TEST_ASSERT_EQUAL(RESCUE_SLOT_UNIDENTIFIED, f.state);
  TEST_ASSERT_EQUAL_STRING("", f.rev);
}

static void test_formatter_all_filtered_rev_is_unidentified() {
  char buf[SLOT_RECORD_BUF_LEN];
  uint8_t sha[32] = {0};
  TEST_ASSERT_TRUE(formatSlotRecord(buf, sizeof(buf), 0, sha, "|||\"|||"));
  RescueSlotFacts f = rescueSlotFacts(true, true, parseSlotRecord(buf), true, "", OWN);
  TEST_ASSERT_EQUAL(RESCUE_SLOT_UNIDENTIFIED, f.state);
}

static void test_formatter_real_rev_round_trips_as_identified() {
  char buf[SLOT_RECORD_BUF_LEN];
  uint8_t sha[32] = {0};
  TEST_ASSERT_TRUE(formatSlotRecord(buf, sizeof(buf), 0, sha, "b0e3fe6"));
  RescueSlotFacts f = rescueSlotFacts(true, true, parseSlotRecord(buf), true, OWN, OWN);
  TEST_ASSERT_TRUE(f.identified);
  TEST_ASSERT_EQUAL_STRING("b0e3fe6", f.rev);
  TEST_ASSERT_EQUAL(RESCUE_SLOT_OK, f.state);
}

// --- the marker in the binary --------------------------------------------------

static void test_the_marker_is_found_in_a_window_and_read_whole() {
  const char image[] = "\xe9\x03junk SFRESCUE-SRC:f14a7b6:END more";
  char head[RESCUE_SOURCE_HEAD_MAX + 1];
  long at = rescueSourceHeadFind((const uint8_t*)image, sizeof(image) - 1, head, sizeof(head));
  TEST_ASSERT_EQUAL(7, at);
  TEST_ASSERT_EQUAL_STRING("f14a7b6", head);
  // A longer head, as a big repository abbreviates.
  const char longer[] = "SFRESCUE-SRC:f14a7b6c9d2:END";
  TEST_ASSERT_EQUAL(0, rescueSourceHeadFind((const uint8_t*)longer, sizeof(longer) - 1, head, sizeof(head)));
  TEST_ASSERT_EQUAL_STRING("f14a7b6c9d2", head);
}

static void test_the_prefix_alone_or_a_cut_marker_is_not_one() {
  char head[RESCUE_SOURCE_HEAD_MAX + 1] = "x";
  // The prefix as a literal of its own, then a real marker after it.
  const char two[] = "SFRESCUE-SRC:\0 SFRESCUE-SRC:0123abc:END";
  TEST_ASSERT_EQUAL(15, rescueSourceHeadFind((const uint8_t*)two, sizeof(two) - 1, head, sizeof(head)));
  TEST_ASSERT_EQUAL_STRING("0123abc", head);
  // Not hex, too short, no suffix, or the window ends inside it.
  const char bad1[] = "SFRESCUE-SRC:zzzzzzz:END";
  const char bad2[] = "SFRESCUE-SRC:abc:END";
  const char bad3[] = "SFRESCUE-SRC:f14a7b6:EN";
  TEST_ASSERT_EQUAL(-1, rescueSourceHeadFind((const uint8_t*)bad1, sizeof(bad1) - 1, head, sizeof(head)));
  TEST_ASSERT_EQUAL(-1, rescueSourceHeadFind((const uint8_t*)bad2, sizeof(bad2) - 1, head, sizeof(head)));
  TEST_ASSERT_EQUAL(-1, rescueSourceHeadFind((const uint8_t*)bad3, sizeof(bad3) - 1, head, sizeof(head)));
  const char cut[] = "SFRESCUE-SRC:f14a7b6:END";
  TEST_ASSERT_EQUAL(-1, rescueSourceHeadFind((const uint8_t*)cut, sizeof(cut) - 3, head, sizeof(head)));
  TEST_ASSERT_EQUAL(-1, rescueSourceHeadFind((const uint8_t*)"", 0, head, sizeof(head)));
}

static void test_an_image_reads_its_own_marker() {
  const char marker[] = RESCUE_SOURCE_MARKER_PREFIX "f14a7b6" RESCUE_SOURCE_MARKER_SUFFIX;
  TEST_ASSERT_EQUAL(7, rescueSourceHeadLen(marker));
  TEST_ASSERT_EQUAL_STRING_LEN("f14a7b6", rescueSourceHeadOfMarker(marker), 7);
  TEST_ASSERT_TRUE(RESCUE_SOURCE_MARKER_MAX >= sizeof(marker) - 1);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_absent_partition_reports_absent);
  RUN_TEST(test_present_but_no_valid_image_reports_empty);
  RUN_TEST(test_invalid_image_never_inherits_a_matching_record_or_marker);
  RUN_TEST(test_the_same_source_head_is_ok_whatever_the_revs);
  RUN_TEST(test_another_source_head_is_outdated_but_not_a_warning);
  RUN_TEST(test_a_marker_identifies_the_image_without_a_record);
  RUN_TEST(test_a_recorded_image_without_a_marker_is_of_unknown_vintage);
  RUN_TEST(test_nothing_known_is_unidentified_and_a_warning);
  RUN_TEST(test_sha_mismatch_drops_the_rev_not_the_marker);
  RUN_TEST(test_a_build_without_its_own_head_cannot_call_a_slot_current);
  RUN_TEST(test_only_recovery_blocking_states_warn);
  RUN_TEST(test_record_with_empty_rev_is_not_identified);
  RUN_TEST(test_formatter_placeholder_rev_round_trips_as_unidentified);
  RUN_TEST(test_formatter_all_filtered_rev_is_unidentified);
  RUN_TEST(test_formatter_real_rev_round_trips_as_identified);
  RUN_TEST(test_the_marker_is_found_in_a_window_and_read_whole);
  RUN_TEST(test_the_prefix_alone_or_a_cut_marker_is_not_one);
  RUN_TEST(test_an_image_reads_its_own_marker);
  return UNITY_END();
}
