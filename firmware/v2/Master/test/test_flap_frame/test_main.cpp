// Host-side tests for FlapFrame.h (#203): the pure text → per-unit letter
// index mapping displayTask feeds to the unit bus, plus the web→unit speed
// mapping. Alignment/truncation semantics are v1's showMessage() contract;
// the one approved deviation is unknown chars → blank (v1 skipped the slot,
// leaving the previous letter standing).

#include <ArduinoFake.h>
#include <unity.h>

#include "../../FlapFrame.h"

void setUp() {}
void tearDown() {}

// --- flapLetterIndex ---------------------------------------------------------

static void test_letter_index_maps_alphabet() {
  TEST_ASSERT_EQUAL(0, flapLetterIndex(' '));
  TEST_ASSERT_EQUAL(1, flapLetterIndex('A'));
  TEST_ASSERT_EQUAL(26, flapLetterIndex('Z'));
  TEST_ASSERT_EQUAL(30, flapLetterIndex('0'));
  TEST_ASSERT_EQUAL(39, flapLetterIndex('9'));
  TEST_ASSERT_EQUAL(44, flapLetterIndex('-'));
}

static void test_letter_index_uppercases_ascii() {
  TEST_ASSERT_EQUAL(1, flapLetterIndex('a'));
  TEST_ASSERT_EQUAL(26, flapLetterIndex('z'));
}

static void test_letter_index_unknown_is_negative() {
  TEST_ASSERT_TRUE(flapLetterIndex('~') < 0);
  TEST_ASSERT_TRUE(flapLetterIndex('\n') < 0);
  TEST_ASSERT_TRUE(flapLetterIndex((char)0xE4) < 0);  // raw 'ä' — web encodes as $
}

// --- flapFrameBuild ----------------------------------------------------------

static void test_frame_exact_fit() {
  uint8_t out[4];
  flapFrameBuild("AB", 2, DisplayAlignment::Left, out);
  TEST_ASSERT_EQUAL(1, out[0]);
  TEST_ASSERT_EQUAL(2, out[1]);
}

static void test_frame_lowercase_input() {
  uint8_t out[2];
  flapFrameBuild("ab", 2, DisplayAlignment::Left, out);
  TEST_ASSERT_EQUAL(1, out[0]);
  TEST_ASSERT_EQUAL(2, out[1]);
}

static void test_frame_unknown_char_becomes_blank() {
  uint8_t out[3];
  flapFrameBuild("A~B", 3, DisplayAlignment::Left, out);
  TEST_ASSERT_EQUAL(1, out[0]);
  TEST_ASSERT_EQUAL(0, out[1]);
  TEST_ASSERT_EQUAL(2, out[2]);
}

static void test_frame_left_pads_right() {
  uint8_t out[4];
  flapFrameBuild("AB", 4, DisplayAlignment::Left, out);
  TEST_ASSERT_EQUAL(1, out[0]);
  TEST_ASSERT_EQUAL(2, out[1]);
  TEST_ASSERT_EQUAL(0, out[2]);
  TEST_ASSERT_EQUAL(0, out[3]);
}

static void test_frame_right_pads_left() {
  uint8_t out[4];
  flapFrameBuild("AB", 4, DisplayAlignment::Right, out);
  TEST_ASSERT_EQUAL(0, out[0]);
  TEST_ASSERT_EQUAL(0, out[1]);
  TEST_ASSERT_EQUAL(1, out[2]);
  TEST_ASSERT_EQUAL(2, out[3]);
}

static void test_frame_center_splits_padding() {
  // v1 centerString: left pad = (width - len) / 2, remainder goes right.
  uint8_t out[5];
  flapFrameBuild("AB", 5, DisplayAlignment::Center, out);
  TEST_ASSERT_EQUAL(0, out[0]);
  TEST_ASSERT_EQUAL(1, out[1]);
  TEST_ASSERT_EQUAL(2, out[2]);
  TEST_ASSERT_EQUAL(0, out[3]);
  TEST_ASSERT_EQUAL(0, out[4]);
}

static void test_frame_truncates_over_width_any_alignment() {
  // v1 parity: over-width text keeps the FIRST `width` chars regardless of
  // alignment (right/centerString truncate before padding).
  uint8_t out[3];
  flapFrameBuild("ABCDEF", 3, DisplayAlignment::Right, out);
  TEST_ASSERT_EQUAL(1, out[0]);
  TEST_ASSERT_EQUAL(2, out[1]);
  TEST_ASSERT_EQUAL(3, out[2]);
  flapFrameBuild("ABCDEF", 3, DisplayAlignment::Center, out);
  TEST_ASSERT_EQUAL(1, out[0]);
  TEST_ASSERT_EQUAL(2, out[1]);
  TEST_ASSERT_EQUAL(3, out[2]);
}

static void test_frame_empty_text_is_all_blanks() {
  uint8_t out[3] = {9, 9, 9};
  flapFrameBuild("", 3, DisplayAlignment::Center, out);
  TEST_ASSERT_EQUAL(0, out[0]);
  TEST_ASSERT_EQUAL(0, out[1]);
  TEST_ASSERT_EQUAL(0, out[2]);
}

static void test_frame_width_one() {
  uint8_t out[1];
  flapFrameBuild("HI", 1, DisplayAlignment::Left, out);
  TEST_ASSERT_EQUAL(8, out[0]);  // 'H'
}

// --- convertSpeedToUnit ------------------------------------------------------

static void test_speed_endpoints() {
  TEST_ASSERT_EQUAL(MIN_SPEED, convertSpeedToUnit(1));
  TEST_ASSERT_EQUAL(MAX_SPEED, convertSpeedToUnit(100));
}

static void test_speed_midpoints_match_v1_map_math() {
  // v1: constrain(1..100) then map(x, 1, 100, MIN_SPEED, MAX_SPEED) —
  // Arduino integer map truncates toward zero.
  TEST_ASSERT_EQUAL(6, convertSpeedToUnit(50));   // (49*11)/99 + 1
  TEST_ASSERT_EQUAL(2, convertSpeedToUnit(10));   // (9*11)/99 + 1
  TEST_ASSERT_EQUAL(11, convertSpeedToUnit(95));  // (94*11)/99 + 1
}

static void test_speed_clamps_out_of_range() {
  TEST_ASSERT_EQUAL(MIN_SPEED, convertSpeedToUnit(0));
  TEST_ASSERT_EQUAL(MIN_SPEED, convertSpeedToUnit(-5));
  TEST_ASSERT_EQUAL(MAX_SPEED, convertSpeedToUnit(150));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_letter_index_maps_alphabet);
  RUN_TEST(test_letter_index_uppercases_ascii);
  RUN_TEST(test_letter_index_unknown_is_negative);
  RUN_TEST(test_frame_exact_fit);
  RUN_TEST(test_frame_lowercase_input);
  RUN_TEST(test_frame_unknown_char_becomes_blank);
  RUN_TEST(test_frame_left_pads_right);
  RUN_TEST(test_frame_right_pads_left);
  RUN_TEST(test_frame_center_splits_padding);
  RUN_TEST(test_frame_truncates_over_width_any_alignment);
  RUN_TEST(test_frame_empty_text_is_all_blanks);
  RUN_TEST(test_frame_width_one);
  RUN_TEST(test_speed_endpoints);
  RUN_TEST(test_speed_midpoints_match_v1_map_math);
  RUN_TEST(test_speed_clamps_out_of_range);
  return UNITY_END();
}
