// Native tests for WallRowEvents.h: the names of a row board's own events.
#include <unity.h>

#include "../../WallRowEvents.h"

void setUp() {}
void tearDown() {}

static void test_the_codes_have_their_names() {
  TEST_ASSERT_EQUAL_STRING("started", wallRowEventName(1));
  TEST_ASSERT_EQUAL_STRING("self-restart", wallRowEventName(2));
  TEST_ASSERT_EQUAL_STRING("low-memory", wallRowEventName(3));
}

static void test_a_code_this_build_does_not_know_has_none() {
  TEST_ASSERT_EQUAL_STRING("?", wallRowEventName(0));
  TEST_ASSERT_EQUAL_STRING("?", wallRowEventName(200));
  TEST_ASSERT_EQUAL_STRING("?", wallRowEventName(70000));
}

static void test_a_code_past_one_byte_is_kept_as_none() {
  TEST_ASSERT_EQUAL_UINT8(3, wallRowEventDetail(3));
  TEST_ASSERT_EQUAL_UINT8(255, wallRowEventDetail(255));
  TEST_ASSERT_EQUAL_UINT8(0, wallRowEventDetail(256));
  TEST_ASSERT_EQUAL_UINT8(0, wallRowEventDetail(0x10003));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_the_codes_have_their_names);
  RUN_TEST(test_a_code_this_build_does_not_know_has_none);
  RUN_TEST(test_a_code_past_one_byte_is_kept_as_none);
  return UNITY_END();
}
