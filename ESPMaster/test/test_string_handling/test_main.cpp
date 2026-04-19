// Host-side unit tests for the pure-logic helpers in HelpersStringHandling.ino.
// The .ino is textually included so the tests exercise the real code.

#include <ArduinoFake.h>
#include <unity.h>
#include "../../Classes.h"
#include "../../HelpersSerialHandling.h"
#include "../../HelpersStringHandling.ino"

using namespace fakeit;

void setUp() {
  ArduinoFakeReset();
  // ArduinoFake stubs `map()` as a mock; wire in the real Arduino formula so
  // convertSpeed (which calls map()) works during tests.
  When(Method(ArduinoFake(Function), map)).AlwaysDo(
    [](long value, long fromLow, long fromHigh, long toLow, long toHigh) -> long {
      return (value - fromLow) * (toHigh - toLow) / (fromHigh - fromLow) + toLow;
    });
}
void tearDown() {}

// isNumber
static void test_isNumber_accepts_positive_integer() {
  TEST_ASSERT_TRUE(isNumber(String("123")));
}
static void test_isNumber_accepts_negative_integer() {
  TEST_ASSERT_TRUE(isNumber(String("-45")));
}
static void test_isNumber_rejects_letters() {
  TEST_ASSERT_FALSE(isNumber(String("abc")));
  TEST_ASSERT_FALSE(isNumber(String("12a")));
}
static void test_isNumber_rejects_empty() {
  TEST_ASSERT_FALSE(isNumber(String("")));
}

// convertSpeed: maps 1..100 -> MIN_SPEED..MAX_SPEED (1..12)
static void test_convertSpeed_endpoints() {
  TEST_ASSERT_EQUAL_INT(MIN_SPEED, convertSpeed(String("1")));
  TEST_ASSERT_EQUAL_INT(MAX_SPEED, convertSpeed(String("100")));
}

// createRepeatingString
static void test_createRepeatingString_length_matches_units() {
  String s = createRepeatingString('-');
  TEST_ASSERT_EQUAL_INT(UNITS_AMOUNT, (int)s.length());
  for (int i = 0; i < UNITS_AMOUNT; i++) {
    TEST_ASSERT_EQUAL_CHAR('-', s[i]);
  }
}

// cleanString uppercases
static void test_cleanString_uppercases() {
  String s = cleanString(String("hello"));
  TEST_ASSERT_EQUAL_STRING("HELLO", s.c_str());
}

// leftString: message followed by padding
static void test_leftString_pads_right() {
  String s = leftString(String("HI"));
  TEST_ASSERT_EQUAL_INT(UNITS_AMOUNT, (int)s.length());
  TEST_ASSERT_EQUAL_STRING("HI        ", s.c_str());
}

// rightString: padding followed by message
static void test_rightString_pads_left() {
  String s = rightString(String("HI"));
  TEST_ASSERT_EQUAL_INT(UNITS_AMOUNT, (int)s.length());
  TEST_ASSERT_EQUAL_STRING("        HI", s.c_str());
}

// centerString: equal padding both sides (left bias on odd remainder)
static void test_centerString_even_padding() {
  String s = centerString(String("HI"));
  TEST_ASSERT_EQUAL_INT(UNITS_AMOUNT, (int)s.length());
  TEST_ASSERT_EQUAL_STRING("    HI    ", s.c_str());
}
static void test_centerString_odd_remainder() {
  String s = centerString(String("HEY"));
  TEST_ASSERT_EQUAL_INT(UNITS_AMOUNT, (int)s.length());
  // (10 - 3) / 2 = 3 left pad, remainder of 4 trailing pad
  TEST_ASSERT_EQUAL_STRING("   HEY    ", s.c_str());
}

// Overflow guard: a message longer than UNITS_AMOUNT must not spin the padding
// loop forever (it used to, because `UNITS_AMOUNT - message.length()` wrapped
// in unsigned arithmetic). It should truncate cleanly instead.
static void test_centerString_longer_than_display() {
  String s = centerString(String("ABCDEFGHIJKL"));
  TEST_ASSERT_EQUAL_INT(UNITS_AMOUNT, (int)s.length());
  TEST_ASSERT_EQUAL_STRING("ABCDEFGHIJ", s.c_str());
}
static void test_leftString_longer_than_display() {
  String s = leftString(String("ABCDEFGHIJKL"));
  TEST_ASSERT_EQUAL_INT(UNITS_AMOUNT, (int)s.length());
  TEST_ASSERT_EQUAL_STRING("ABCDEFGHIJ", s.c_str());
}
static void test_rightString_longer_than_display() {
  String s = rightString(String("ABCDEFGHIJKL"));
  TEST_ASSERT_EQUAL_INT(UNITS_AMOUNT, (int)s.length());
  TEST_ASSERT_EQUAL_STRING("ABCDEFGHIJ", s.c_str());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_isNumber_accepts_positive_integer);
  RUN_TEST(test_isNumber_accepts_negative_integer);
  RUN_TEST(test_isNumber_rejects_letters);
  RUN_TEST(test_isNumber_rejects_empty);
  RUN_TEST(test_convertSpeed_endpoints);
  RUN_TEST(test_createRepeatingString_length_matches_units);
  RUN_TEST(test_cleanString_uppercases);
  RUN_TEST(test_leftString_pads_right);
  RUN_TEST(test_rightString_pads_left);
  RUN_TEST(test_centerString_even_padding);
  RUN_TEST(test_centerString_odd_remainder);
  RUN_TEST(test_centerString_longer_than_display);
  RUN_TEST(test_leftString_longer_than_display);
  RUN_TEST(test_rightString_longer_than_display);
  return UNITY_END();
}
