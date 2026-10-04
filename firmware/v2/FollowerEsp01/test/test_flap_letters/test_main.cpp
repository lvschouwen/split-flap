// Host-side tests for the shared FlapLetters.h as the ESP-01 row uses it
// (#528): a segment from the leader arrives un-normalised, so the mapping
// itself must uppercase, and a character the drum lacks must blank the unit.

#include <unity.h>

#include "FlapLetters.h"

void setUp() {}
void tearDown() {}

static void test_lowercase_maps_like_uppercase() {
  const char* date = "13 Jul 26";
  const char* upper = "13 JUL 26";
  for (int i = 0; date[i] != '\0'; i++) {
    TEST_ASSERT_EQUAL(flapLetterIndex(upper[i]), flapLetterIndex(date[i]));
    TEST_ASSERT_TRUE(flapLetterIndex(date[i]) >= 0);
  }
}

static void test_unknown_character_blanks_the_unit() {
  TEST_ASSERT_TRUE(flapLetterIndex('~') < 0);
  TEST_ASSERT_EQUAL(0, flapLetterOrBlank('~'));
  TEST_ASSERT_EQUAL(0, flapLetterOrBlank(' '));
  TEST_ASSERT_EQUAL(1, flapLetterOrBlank('a'));
}

static void test_speed_range() {
  TEST_ASSERT_EQUAL(MIN_SPEED, convertSpeedToUnit(1));
  TEST_ASSERT_EQUAL(MAX_SPEED, convertSpeedToUnit(100));
  TEST_ASSERT_EQUAL(6, convertSpeedToUnit(50));
  TEST_ASSERT_EQUAL(MIN_SPEED, convertSpeedToUnit(-3));
  TEST_ASSERT_EQUAL(MAX_SPEED, convertSpeedToUnit(400));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_lowercase_maps_like_uppercase);
  RUN_TEST(test_unknown_character_blanks_the_unit);
  RUN_TEST(test_speed_range);
  return UNITY_END();
}
