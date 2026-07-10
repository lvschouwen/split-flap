// Host-side unit tests for RescueSlots.h (#195) — pure slot-choice logic
// behind POST /rescue/exit. The glue reads esp_app_desc_t (whose date/time
// carry the image's __DATE__/__TIME__) for each OTA slot; these functions
// turn that into "which slot do we boot back into": the newest valid one.

#include <ArduinoFake.h>
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

// --- pickExitSlot -------------------------------------------------------------

static void test_pick_only_slot0_valid() {
  TEST_ASSERT_EQUAL(0, pickExitSlot(true, 100, false, 200));
}

static void test_pick_only_slot1_valid() {
  TEST_ASSERT_EQUAL(1, pickExitSlot(false, 200, true, 100));
}

static void test_pick_none_valid() {
  TEST_ASSERT_EQUAL(-1, pickExitSlot(false, 0, false, 0));
}

static void test_pick_newest_of_two_valid() {
  TEST_ASSERT_EQUAL(1, pickExitSlot(true, 100, true, 200));
  TEST_ASSERT_EQUAL(0, pickExitSlot(true, 200, true, 100));
}

static void test_pick_tie_prefers_slot0() {
  // Includes the both-stamps-unparseable (0,0) case: deterministic fallback.
  TEST_ASSERT_EQUAL(0, pickExitSlot(true, 100, true, 100));
  TEST_ASSERT_EQUAL(0, pickExitSlot(true, 0, true, 0));
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
  RUN_TEST(test_pick_only_slot0_valid);
  RUN_TEST(test_pick_only_slot1_valid);
  RUN_TEST(test_pick_none_valid);
  RUN_TEST(test_pick_newest_of_two_valid);
  RUN_TEST(test_pick_tie_prefers_slot0);
  return UNITY_END();
}
