// Host-side unit tests for the pure-logic helpers in HelpersStringHandling.ino.
// The .ino is textually included so the tests exercise the real code.

#include <ArduinoFake.h>
#include <unity.h>
#include "../../HelpersSerialHandling.h"

// Effective display width (#123): defined by ESPMaster.ino in firmware; the
// test provides it here since HelpersStringHandling.ino only declares it.
int displayWidth = UNITS_AMOUNT;

#include "../../HelpersStringHandling.ino"

using namespace fakeit;

void setUp() {
  ArduinoFakeReset();
  displayWidth = UNITS_AMOUNT;
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

// Out-of-range and garbage input must clamp, never extrapolate (#95).
// map() extrapolates linearly, so pre-fix "500" produced ~56 — a speed
// byte far beyond anything the unit stepper can execute.
static void test_convertSpeed_clamps_out_of_range() {
  TEST_ASSERT_EQUAL_INT(MAX_SPEED, convertSpeed(String("101")));
  TEST_ASSERT_EQUAL_INT(MAX_SPEED, convertSpeed(String("500")));
  TEST_ASSERT_EQUAL_INT(MIN_SPEED, convertSpeed(String("0")));
  TEST_ASSERT_EQUAL_INT(MIN_SPEED, convertSpeed(String("-50")));
  TEST_ASSERT_EQUAL_INT(MIN_SPEED, convertSpeed(String("abc")));  // toInt() == 0
  TEST_ASSERT_EQUAL_INT(MIN_SPEED, convertSpeed(String("")));
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

// The alignment tests below pin displayWidth to 10 so the expected strings
// stay literal — the helpers follow displayWidth, not UNITS_AMOUNT (#123).

// leftString: message followed by padding
static void test_leftString_pads_right() {
  displayWidth = 10;
  String s = leftString(String("HI"));
  TEST_ASSERT_EQUAL_INT(10, (int)s.length());
  TEST_ASSERT_EQUAL_STRING("HI        ", s.c_str());
}

// rightString: padding followed by message
static void test_rightString_pads_left() {
  displayWidth = 10;
  String s = rightString(String("HI"));
  TEST_ASSERT_EQUAL_INT(10, (int)s.length());
  TEST_ASSERT_EQUAL_STRING("        HI", s.c_str());
}

// centerString: equal padding both sides (left bias on odd remainder)
static void test_centerString_even_padding() {
  displayWidth = 10;
  String s = centerString(String("HI"));
  TEST_ASSERT_EQUAL_INT(10, (int)s.length());
  TEST_ASSERT_EQUAL_STRING("    HI    ", s.c_str());
}
static void test_centerString_odd_remainder() {
  displayWidth = 10;
  String s = centerString(String("HEY"));
  TEST_ASSERT_EQUAL_INT(10, (int)s.length());
  // (10 - 3) / 2 = 3 left pad, remainder of 4 trailing pad
  TEST_ASSERT_EQUAL_STRING("   HEY    ", s.c_str());
}

// Overflow guard: a message longer than the display must not spin the padding
// loop forever (it used to, because `width - message.length()` wrapped
// in unsigned arithmetic). It should truncate cleanly instead.
static void test_centerString_longer_than_display() {
  displayWidth = 10;
  String s = centerString(String("ABCDEFGHIJKL"));
  TEST_ASSERT_EQUAL_INT(10, (int)s.length());
  TEST_ASSERT_EQUAL_STRING("ABCDEFGHIJ", s.c_str());
}
static void test_leftString_longer_than_display() {
  displayWidth = 10;
  String s = leftString(String("ABCDEFGHIJKL"));
  TEST_ASSERT_EQUAL_INT(10, (int)s.length());
  TEST_ASSERT_EQUAL_STRING("ABCDEFGHIJ", s.c_str());
}
static void test_rightString_longer_than_display() {
  displayWidth = 10;
  String s = rightString(String("ABCDEFGHIJKL"));
  TEST_ASSERT_EQUAL_INT(10, (int)s.length());
  TEST_ASSERT_EQUAL_STRING("ABCDEFGHIJ", s.c_str());
}

// Dynamic display width (#123): all layout helpers must follow the probed
// width, not the UNITS_AMOUNT compile-time ceiling — a 5-unit display used
// to center text across the full virtual width, pushing characters onto
// absent units.
static void test_leftString_uses_display_width() {
  displayWidth = 5;
  String s = leftString(String("AB"));
  TEST_ASSERT_EQUAL_STRING("AB   ", s.c_str());
}
static void test_rightString_uses_display_width() {
  displayWidth = 5;
  String s = rightString(String("AB"));
  TEST_ASSERT_EQUAL_STRING("   AB", s.c_str());
}
static void test_centerString_uses_display_width() {
  displayWidth = 5;
  String s = centerString(String("AB"));
  TEST_ASSERT_EQUAL_STRING(" AB  ", s.c_str());
}
static void test_createRepeatingString_uses_display_width() {
  displayWidth = 5;
  String s = createRepeatingString('.');
  TEST_ASSERT_EQUAL_STRING(".....", s.c_str());
}
static void test_alignment_truncates_at_display_width() {
  displayWidth = 5;
  TEST_ASSERT_EQUAL_STRING("ABCDE", leftString(String("ABCDEFG")).c_str());
  TEST_ASSERT_EQUAL_STRING("ABCDE", rightString(String("ABCDEFG")).c_str());
  TEST_ASSERT_EQUAL_STRING("ABCDE", centerString(String("ABCDEFG")).c_str());
}
static void test_processSentenceToLines_wraps_at_display_width() {
  displayWidth = 5;
  std::vector<String> lines = processSentenceToLines(String("HELLO WORLD"));
  TEST_ASSERT_EQUAL_INT(2, (int)lines.size());
  TEST_ASSERT_EQUAL_STRING("HELLO", lines[0].c_str());
  TEST_ASSERT_EQUAL_STRING("WORLD", lines[1].c_str());
}
static void test_processSentenceToLines_hyphenates_at_display_width() {
  displayWidth = 5;
  std::vector<String> lines = processSentenceToLines(String("ABCDEFG"));
  TEST_ASSERT_EQUAL_INT(2, (int)lines.size());
  TEST_ASSERT_EQUAL_STRING("ABCD-", lines[0].c_str());
  TEST_ASSERT_EQUAL_STRING("EFG", lines[1].c_str());
}
static void test_single_unit_display_width() {
  displayWidth = 1;
  TEST_ASSERT_EQUAL_STRING("A", leftString(String("ABC")).c_str());
  TEST_ASSERT_EQUAL_STRING("X", centerString(String("X")).c_str());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_isNumber_accepts_positive_integer);
  RUN_TEST(test_isNumber_accepts_negative_integer);
  RUN_TEST(test_isNumber_rejects_letters);
  RUN_TEST(test_isNumber_rejects_empty);
  RUN_TEST(test_convertSpeed_endpoints);
  RUN_TEST(test_convertSpeed_clamps_out_of_range);
  RUN_TEST(test_createRepeatingString_length_matches_units);
  RUN_TEST(test_cleanString_uppercases);
  RUN_TEST(test_leftString_pads_right);
  RUN_TEST(test_rightString_pads_left);
  RUN_TEST(test_centerString_even_padding);
  RUN_TEST(test_centerString_odd_remainder);
  RUN_TEST(test_centerString_longer_than_display);
  RUN_TEST(test_leftString_longer_than_display);
  RUN_TEST(test_rightString_longer_than_display);
  RUN_TEST(test_leftString_uses_display_width);
  RUN_TEST(test_rightString_uses_display_width);
  RUN_TEST(test_centerString_uses_display_width);
  RUN_TEST(test_createRepeatingString_uses_display_width);
  RUN_TEST(test_alignment_truncates_at_display_width);
  RUN_TEST(test_processSentenceToLines_wraps_at_display_width);
  RUN_TEST(test_processSentenceToLines_hyphenates_at_display_width);
  RUN_TEST(test_single_unit_display_width);
  return UNITY_END();
}
