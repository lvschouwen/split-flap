// Host-side unit tests for RescueOta.h (#195) — upload gating for the rescue
// app's POST /firmware/master. normalizeOtaMd5 is a COPY of Master's
// OtaStatus.h helper (repo convention: rescue shares no compiled code with
// Master), so the wire contract is re-pinned here against the copy.

#include <ArduinoFake.h>
#include <unity.h>

#include "../../RescueOta.h"

void setUp() {}
void tearDown() {}

static void test_md5_lowercase_hex_accepted() {
  String md5 = "0123456789abcdef0123456789abcdef";
  TEST_ASSERT_TRUE(normalizeOtaMd5(md5));
  TEST_ASSERT_EQUAL_STRING("0123456789abcdef0123456789abcdef", md5.c_str());
}

static void test_md5_uppercase_normalized_in_place() {
  String md5 = "0123456789ABCDEF0123456789ABCDEF";
  TEST_ASSERT_TRUE(normalizeOtaMd5(md5));
  TEST_ASSERT_EQUAL_STRING("0123456789abcdef0123456789abcdef", md5.c_str());
}

static void test_md5_wrong_length_rejected() {
  String short31 = "0123456789abcdef0123456789abcde";
  String long33 = "0123456789abcdef0123456789abcdef0";
  String empty = "";
  TEST_ASSERT_FALSE(normalizeOtaMd5(short31));
  TEST_ASSERT_FALSE(normalizeOtaMd5(long33));
  TEST_ASSERT_FALSE(normalizeOtaMd5(empty));
}

static void test_md5_non_hex_rejected() {
  String bad = "0123456789abcdef0123456789abcdeg";
  TEST_ASSERT_FALSE(normalizeOtaMd5(bad));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_md5_lowercase_hex_accepted);
  RUN_TEST(test_md5_uppercase_normalized_in_place);
  RUN_TEST(test_md5_wrong_length_rejected);
  RUN_TEST(test_md5_non_hex_rejected);
  return UNITY_END();
}
