// Host-side unit tests for the pure-logic helpers in HelpersStringHandling.ino.
// The .ino is textually included so the tests exercise the real code.

#include <ArduinoFake.h>
#include <unity.h>
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

// Padding-test helpers. Computing expectations from UNITS_AMOUNT instead of
// hardcoding string literals keeps the tests valid across display-width
// bumps (#51 raised UNITS_AMOUNT from 10 to 16; this test previously broke
// because it hardcoded the 10-wide layout).
static String padLeftExpected(const char* msg) {
  String s = msg;
  while ((int)s.length() < UNITS_AMOUNT) s += ' ';
  return s;
}
static String padRightExpected(const char* msg) {
  String s;
  while ((int)s.length() < UNITS_AMOUNT - (int)strlen(msg)) s += ' ';
  s += msg;
  return s;
}
static String padCenterExpected(const char* msg) {
  int mlen = (int)strlen(msg);
  int slack = UNITS_AMOUNT - mlen;
  int leftPad = slack / 2;              // centerString left-biases
  int rightPad = slack - leftPad;
  String s;
  for (int i = 0; i < leftPad; i++) s += ' ';
  s += msg;
  for (int i = 0; i < rightPad; i++) s += ' ';
  return s;
}

// leftString: message followed by padding
static void test_leftString_pads_right() {
  String s = leftString(String("HI"));
  TEST_ASSERT_EQUAL_INT(UNITS_AMOUNT, (int)s.length());
  TEST_ASSERT_EQUAL_STRING(padLeftExpected("HI").c_str(), s.c_str());
}

// rightString: padding followed by message
static void test_rightString_pads_left() {
  String s = rightString(String("HI"));
  TEST_ASSERT_EQUAL_INT(UNITS_AMOUNT, (int)s.length());
  TEST_ASSERT_EQUAL_STRING(padRightExpected("HI").c_str(), s.c_str());
}

// centerString: equal padding both sides (left bias on odd remainder)
static void test_centerString_even_padding() {
  String s = centerString(String("HI"));
  TEST_ASSERT_EQUAL_INT(UNITS_AMOUNT, (int)s.length());
  TEST_ASSERT_EQUAL_STRING(padCenterExpected("HI").c_str(), s.c_str());
}
static void test_centerString_odd_remainder() {
  String s = centerString(String("HEY"));
  TEST_ASSERT_EQUAL_INT(UNITS_AMOUNT, (int)s.length());
  //Odd slack splits as floor/ceil — e.g. UNITS_AMOUNT=10 -> 3/4; =16 -> 6/7.
  TEST_ASSERT_EQUAL_STRING(padCenterExpected("HEY").c_str(), s.c_str());
}

// Overflow guard: a message longer than UNITS_AMOUNT must not spin the padding
// loop forever (it used to, because `UNITS_AMOUNT - message.length()` wrapped
// in unsigned arithmetic). Input must stay longer than any plausible future
// UNITS_AMOUNT so the truncation branch is the one we're exercising.
// UNITS_AMOUNT is bounded above by the 4-bit DIP scheme = 16, so a 32-char
// input is safely longer for any realistic configuration.
static const char kTooLong[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ012345";  // 32 chars
static String firstNChars(const char* s, int n) {
  String out;
  for (int i = 0; i < n && s[i]; i++) out += s[i];
  return out;
}
static void test_centerString_longer_than_display() {
  String s = centerString(String(kTooLong));
  TEST_ASSERT_EQUAL_INT(UNITS_AMOUNT, (int)s.length());
  TEST_ASSERT_EQUAL_STRING(firstNChars(kTooLong, UNITS_AMOUNT).c_str(), s.c_str());
}
static void test_leftString_longer_than_display() {
  String s = leftString(String(kTooLong));
  TEST_ASSERT_EQUAL_INT(UNITS_AMOUNT, (int)s.length());
  TEST_ASSERT_EQUAL_STRING(firstNChars(kTooLong, UNITS_AMOUNT).c_str(), s.c_str());
}
static void test_rightString_longer_than_display() {
  String s = rightString(String(kTooLong));
  TEST_ASSERT_EQUAL_INT(UNITS_AMOUNT, (int)s.length());
  TEST_ASSERT_EQUAL_STRING(firstNChars(kTooLong, UNITS_AMOUNT).c_str(), s.c_str());
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
