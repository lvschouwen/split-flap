// Host-side tests for DisplayEvents.h (#251) — SSE display push change
// detection and payload. The AsyncEventSource glue in WebEndpoints.cpp is
// bench tier.

#include <ArduinoFake.h>
#include <unity.h>

#include <cstring>

#include "../../DisplayEvents.h"

void setUp() {}
void tearDown() {}

static void test_first_nonempty_text_is_due_once() {
  DisplayEventTracker t;
  TEST_ASSERT_TRUE(displayEventDue(t, "18:56"));
  TEST_ASSERT_FALSE(displayEventDue(t, "18:56"));  // unchanged — no push
}

static void test_empty_initial_text_is_not_due() {
  DisplayEventTracker t;
  TEST_ASSERT_FALSE(displayEventDue(t, ""));  // tracker starts at ""
}

static void test_change_and_stop_clear_are_due() {
  DisplayEventTracker t;
  TEST_ASSERT_TRUE(displayEventDue(t, "18:56"));
  TEST_ASSERT_TRUE(displayEventDue(t, "18:57"));
  TEST_ASSERT_TRUE(displayEventDue(t, ""));  // Stop clears the text
  TEST_ASSERT_FALSE(displayEventDue(t, ""));
}

static void test_overlong_text_is_truncated_not_overflowed() {
  DisplayEventTracker t;
  char big[DISPLAY_CMD_TEXT_LEN * 2];
  memset(big, 'A', sizeof(big) - 1);
  big[sizeof(big) - 1] = '\0';
  TEST_ASSERT_TRUE(displayEventDue(t, big));
  TEST_ASSERT_EQUAL(DISPLAY_CMD_TEXT_LEN, (int)strlen(t.lastText));
  // Same overlong text again: identical within the tracked window.
  TEST_ASSERT_FALSE(displayEventDue(t, big));
}

static void test_payload_shape_and_escaping() {
  String json = buildDisplayEventJson("A\"B\\C");
  TEST_ASSERT_EQUAL_STRING("{\"text\":\"A\\\"B\\\\C\"}", json.c_str());
}

static void test_payload_empty_text() {
  String json = buildDisplayEventJson("");
  TEST_ASSERT_EQUAL_STRING("{\"text\":\"\"}", json.c_str());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_first_nonempty_text_is_due_once);
  RUN_TEST(test_empty_initial_text_is_not_due);
  RUN_TEST(test_change_and_stop_clear_are_due);
  RUN_TEST(test_overlong_text_is_truncated_not_overflowed);
  RUN_TEST(test_payload_shape_and_escaping);
  RUN_TEST(test_payload_empty_text);
  UNITY_END();
  return 0;
}
