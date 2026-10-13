// Native tests for TextTransliterate.h (#618): accents become flaps.
#include <ArduinoFake.h>
#include <unity.h>

#include "../../TextTransliterate.h"

void setUp() {}
void tearDown() {}

static void test_plain_text_is_left_alone_but_uppercased() {
  TEST_ASSERT_EQUAL_STRING("HELLO 12:30!", transliterateForFlaps("HELLO 12:30!").c_str());
  TEST_ASSERT_EQUAL_STRING("", transliterateForFlaps("").c_str());
  TEST_ASSERT_EQUAL_STRING("A\nB", transliterateForFlaps("a\nb").c_str());
}

static void test_dutch_names_keep_their_letters() {
  TEST_ASSERT_EQUAL_STRING("ZOE EN NOEL", transliterateForFlaps("Zoë en Noël").c_str());
  TEST_ASSERT_EQUAL_STRING("CAFE RENE", transliterateForFlaps("Café René").c_str());
  TEST_ASSERT_EQUAL_STRING("ANDRE", transliterateForFlaps("André").c_str());
  TEST_ASSERT_EQUAL_STRING("IJSBAAN", transliterateForFlaps("Ĳsbaan").c_str());
}

static void test_umlauts_become_the_drums_own_flaps() {
  TEST_ASSERT_EQUAL_STRING("M$RZ", transliterateForFlaps("März").c_str());
  TEST_ASSERT_EQUAL_STRING("K&LN", transliterateForFlaps("Köln").c_str());
  TEST_ASSERT_EQUAL_STRING("#BER", transliterateForFlaps("Über").c_str());
  TEST_ASSERT_EQUAL_STRING("STRASSE", transliterateForFlaps("Straße").c_str());
  TEST_ASSERT_EQUAL_STRING("AEOE", transliterateForFlaps("Æœ").c_str());
}

static void test_what_has_no_letter_passes_through() {
  // × ÷ and anything outside Latin stay as they are: a blank on the drum.
  TEST_ASSERT_EQUAL_STRING("2×3", transliterateForFlaps("2×3").c_str());
  TEST_ASSERT_EQUAL_STRING("日本", transliterateForFlaps("日本").c_str());
  // A lone lead byte is not a sequence.
  TEST_ASSERT_EQUAL_STRING("\xC3", transliterateForFlaps("\xC3").c_str());
}

static void test_unsupported_counts_characters_without_a_flap() {
  TEST_ASSERT_EQUAL(0, unsupportedFlapCount("HELLO\nworld 12:30!?-."));
  TEST_ASSERT_EQUAL(1, unsupportedFlapCount("IT'S"));
  TEST_ASSERT_EQUAL(2, unsupportedFlapCount("A,B;"));
  // A multi-byte character counts once.
  TEST_ASSERT_EQUAL(2, unsupportedFlapCount("日本"));
  TEST_ASSERT_EQUAL(0, unsupportedFlapCount(transliterateForFlaps("Zoë")));
  TEST_ASSERT_EQUAL(1, unsupportedFlapCount("Zoë"));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_plain_text_is_left_alone_but_uppercased);
  RUN_TEST(test_dutch_names_keep_their_letters);
  RUN_TEST(test_umlauts_become_the_drums_own_flaps);
  RUN_TEST(test_what_has_no_letter_passes_through);
  RUN_TEST(test_unsupported_counts_characters_without_a_flap);
  return UNITY_END();
}
