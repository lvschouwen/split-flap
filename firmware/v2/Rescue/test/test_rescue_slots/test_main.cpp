// Host-side unit tests for RescueSlots.h (#195) — pure slot-choice logic
// behind POST /rescue/exit. The glue reads esp_app_desc_t (whose date/time
// carry the image's __DATE__/__TIME__) for each OTA slot; these functions
// turn that into "which slot do we boot back into": the newest valid one.

#include <ArduinoFake.h>
#include <stdio.h>
#include <unity.h>

#include "../../RescueSlots.h"

void setUp() {}
void tearDown() {}

// --- parseAppBuildStamp -------------------------------------------------------

static void test_stamp_orders_by_day() {
  TEST_ASSERT_TRUE(parseAppBuildStamp("Jul 10 2026", "17:35:42") >
                   parseAppBuildStamp("Jul  9 2026", "23:59:59"));
}

static void test_stamp_orders_by_time_same_day() {
  TEST_ASSERT_TRUE(parseAppBuildStamp("Jul 10 2026", "17:35:43") >
                   parseAppBuildStamp("Jul 10 2026", "17:35:42"));
}

static void test_stamp_year_dominates_month() {
  TEST_ASSERT_TRUE(parseAppBuildStamp("Jan  1 2027", "00:00:00") >
                   parseAppBuildStamp("Dec 31 2026", "23:59:59"));
}

static void test_stamp_month_ordering() {
  TEST_ASSERT_TRUE(parseAppBuildStamp("Dec  1 2026", "00:00:00") >
                   parseAppBuildStamp("Jan  1 2026", "00:00:00"));
}

static void test_stamp_space_padded_day_parses() {
  // __DATE__ pads single-digit days with a space, not a zero.
  TEST_ASSERT_TRUE(parseAppBuildStamp("Jul  1 2026", "12:00:00") != 0);
}

static void test_stamp_equal_inputs_equal() {
  TEST_ASSERT_EQUAL_UINT64(parseAppBuildStamp("Jul 10 2026", "17:35:42"),
                           parseAppBuildStamp("Jul 10 2026", "17:35:42"));
}

static void test_stamp_rejects_bad_month() {
  TEST_ASSERT_EQUAL_UINT64(0, parseAppBuildStamp("Xyz 10 2026", "17:35:42"));
}

static void test_stamp_rejects_short_or_garbage() {
  TEST_ASSERT_EQUAL_UINT64(0, parseAppBuildStamp("", ""));
  TEST_ASSERT_EQUAL_UINT64(0, parseAppBuildStamp("Jul 10", "17:35:42"));
  TEST_ASSERT_EQUAL_UINT64(0, parseAppBuildStamp("Jul 10 2026", "1735:42"));
  TEST_ASSERT_EQUAL_UINT64(0, parseAppBuildStamp("Jul xx 2026", "17:35:42"));
  TEST_ASSERT_EQUAL_UINT64(0, parseAppBuildStamp(nullptr, nullptr));
}

// The exact encoding and every field's bounds, pinned so a restructured
// parser keeps the same values (the exit choice compares them).
static uint64_t stampOf(int year, int month, int day, int h, int m, int sec) {
  return (((((uint64_t)year * 12 + (month - 1)) * 32 + day) * 24 + h) * 60 + m) * 60 + sec;
}

static void test_stamp_exact_encoding() {
  TEST_ASSERT_EQUAL_UINT64(stampOf(2026, 7, 10, 17, 35, 42),
                           parseAppBuildStamp("Jul 10 2026", "17:35:42"));
  TEST_ASSERT_EQUAL_UINT64(stampOf(2026, 7, 1, 12, 0, 0),
                           parseAppBuildStamp("Jul  1 2026", "12:00:00"));
  TEST_ASSERT_EQUAL_UINT64(stampOf(1999, 12, 31, 23, 59, 59),
                           parseAppBuildStamp("Dec 31 1999", "23:59:59"));
  TEST_ASSERT_EQUAL_UINT64(stampOf(2030, 1, 9, 0, 0, 0),
                           parseAppBuildStamp("Jan 09 2030", "00:00:00"));
}

static void test_stamp_every_month_parses() {
  static const char* names[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  for (int m = 0; m < 12; m++) {
    char date[12];
    snprintf(date, sizeof(date), "%s 15 2026", names[m]);
    TEST_ASSERT_EQUAL_UINT64(stampOf(2026, m + 1, 15, 8, 0, 0),
                             parseAppBuildStamp(date, "08:00:00"));
  }
}

static void test_stamp_rejects_each_bad_field() {
  const char* t = "17:35:42";
  const char* d = "Jul 10 2026";
  TEST_ASSERT_EQUAL_UINT64(0, parseAppBuildStamp("Jul 10 20266", t));  // too long
  TEST_ASSERT_EQUAL_UINT64(0, parseAppBuildStamp("jul 10 2026", t));   // case
  TEST_ASSERT_EQUAL_UINT64(0, parseAppBuildStamp("Jul-10 2026", t));   // separator
  TEST_ASSERT_EQUAL_UINT64(0, parseAppBuildStamp("Jul 10-2026", t));
  TEST_ASSERT_EQUAL_UINT64(0, parseAppBuildStamp("Jul  0 2026", t));   // day 0
  TEST_ASSERT_EQUAL_UINT64(0, parseAppBuildStamp("Jul 00 2026", t));
  TEST_ASSERT_EQUAL_UINT64(0, parseAppBuildStamp("Jul 32 2026", t));   // day 32
  TEST_ASSERT_EQUAL_UINT64(0, parseAppBuildStamp("Jul x1 2026", t));
  TEST_ASSERT_EQUAL_UINT64(0, parseAppBuildStamp("Jul 1  2026", t));
  TEST_ASSERT_EQUAL_UINT64(0, parseAppBuildStamp("Jul 10 20x6", t));
  TEST_ASSERT_EQUAL_UINT64(0, parseAppBuildStamp(d, "24:00:00"));      // hour
  TEST_ASSERT_EQUAL_UINT64(0, parseAppBuildStamp(d, "23:60:00"));      // minute
  TEST_ASSERT_EQUAL_UINT64(0, parseAppBuildStamp(d, "23:59:60"));      // second
  TEST_ASSERT_EQUAL_UINT64(0, parseAppBuildStamp(d, "17:35:42 "));     // too long
  TEST_ASSERT_EQUAL_UINT64(0, parseAppBuildStamp(d, "17:35"));         // too short
  TEST_ASSERT_EQUAL_UINT64(0, parseAppBuildStamp(d, "17.35:42"));
  TEST_ASSERT_EQUAL_UINT64(0, parseAppBuildStamp(d, "17:35.42"));
  TEST_ASSERT_EQUAL_UINT64(0, parseAppBuildStamp(d, "1a:35:42"));
  TEST_ASSERT_EQUAL_UINT64(0, parseAppBuildStamp(d, "17:35:4z"));
  TEST_ASSERT_EQUAL_UINT64(0, parseAppBuildStamp(d, nullptr));
  TEST_ASSERT_EQUAL_UINT64(0, parseAppBuildStamp(nullptr, t));
}

// --- pickExitSlot -------------------------------------------------------------

static ExitSlotCandidate cand(bool valid, uint64_t stamp,
                              bool confirmed = false, uint32_t seq = 0) {
  ExitSlotCandidate c;
  c.valid = valid;
  c.stamp = stamp;
  c.confirmed = confirmed;
  c.seq = seq;
  return c;
}

static void test_pick_only_slot0_valid() {
  TEST_ASSERT_EQUAL(0, pickExitSlot(cand(true, 100), cand(false, 200)));
}

static void test_pick_only_slot1_valid() {
  TEST_ASSERT_EQUAL(1, pickExitSlot(cand(false, 200), cand(true, 100)));
}

static void test_pick_none_valid() {
  TEST_ASSERT_EQUAL(-1, pickExitSlot(cand(false, 0), cand(false, 0)));
}

static void test_pick_newest_of_two_valid() {
  TEST_ASSERT_EQUAL(1, pickExitSlot(cand(true, 100), cand(true, 200)));
  TEST_ASSERT_EQUAL(0, pickExitSlot(cand(true, 200), cand(true, 100)));
}

static void test_pick_tie_prefers_slot0() {
  // Includes the both-stamps-unparseable (0,0) case: deterministic fallback.
  TEST_ASSERT_EQUAL(0, pickExitSlot(cand(true, 100), cand(true, 100)));
  TEST_ASSERT_EQUAL(0, pickExitSlot(cand(true, 0), cand(true, 0)));
}

// #200: NVS confirm records outrank the descriptor stamp — under pioarduino
// hybrid builds every image carries the same frozen stamp, so the stamp is
// only a last-resort fallback.

static void test_pick_higher_confirm_seq_wins() {
  TEST_ASSERT_EQUAL(1, pickExitSlot(cand(true, 100, true, 3),
                                    cand(true, 100, true, 7)));
  TEST_ASSERT_EQUAL(0, pickExitSlot(cand(true, 100, true, 7),
                                    cand(true, 100, true, 3)));
}

static void test_pick_seq_beats_newer_stamp() {
  // A deliberate downgrade: the most recently confirmed image wins even
  // though the other slot's image was built later.
  TEST_ASSERT_EQUAL(0, pickExitSlot(cand(true, 100, true, 7),
                                    cand(true, 999, true, 3)));
}

static void test_pick_confirmed_beats_unconfirmed() {
  // A record means "Master ran healthy here"; a slot without one is either
  // pre-#200 firmware or a hand-flashed unknown — known-good wins.
  TEST_ASSERT_EQUAL(1, pickExitSlot(cand(true, 999), cand(true, 100, true, 1)));
  TEST_ASSERT_EQUAL(0, pickExitSlot(cand(true, 100, true, 1), cand(true, 999)));
}

static void test_pick_seq_tie_prefers_slot0() {
  // Two records can only tie if NVS was hand-edited; stay deterministic.
  TEST_ASSERT_EQUAL(0, pickExitSlot(cand(true, 100, true, 5),
                                    cand(true, 200, true, 5)));
}

static void test_pick_record_on_invalid_slot_is_ignored() {
  TEST_ASSERT_EQUAL(1, pickExitSlot(cand(false, 0, true, 9), cand(true, 100)));
  TEST_ASSERT_EQUAL(-1, pickExitSlot(cand(false, 0, true, 9), cand(false, 0)));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_stamp_orders_by_day);
  RUN_TEST(test_stamp_orders_by_time_same_day);
  RUN_TEST(test_stamp_year_dominates_month);
  RUN_TEST(test_stamp_month_ordering);
  RUN_TEST(test_stamp_space_padded_day_parses);
  RUN_TEST(test_stamp_equal_inputs_equal);
  RUN_TEST(test_stamp_rejects_bad_month);
  RUN_TEST(test_stamp_rejects_short_or_garbage);
  RUN_TEST(test_stamp_exact_encoding);
  RUN_TEST(test_stamp_every_month_parses);
  RUN_TEST(test_stamp_rejects_each_bad_field);
  RUN_TEST(test_pick_only_slot0_valid);
  RUN_TEST(test_pick_only_slot1_valid);
  RUN_TEST(test_pick_none_valid);
  RUN_TEST(test_pick_newest_of_two_valid);
  RUN_TEST(test_pick_tie_prefers_slot0);
  RUN_TEST(test_pick_higher_confirm_seq_wins);
  RUN_TEST(test_pick_seq_beats_newer_stamp);
  RUN_TEST(test_pick_confirmed_beats_unconfirmed);
  RUN_TEST(test_pick_seq_tie_prefers_slot0);
  RUN_TEST(test_pick_record_on_invalid_slot_is_ignored);
  return UNITY_END();
}
