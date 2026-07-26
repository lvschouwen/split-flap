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
  String json = buildDisplayEventJson("A\"B\\C", DisplaySource::Web, 0);
  TEST_ASSERT_EQUAL_STRING(
      "{\"text\":\"A\\\"B\\\\C\",\"source\":\"web\",\"sourceAge\":0}",
      json.c_str());
}

static void test_payload_empty_text() {
  String json = buildDisplayEventJson("", DisplaySource::Unknown, 0);
  TEST_ASSERT_EQUAL_STRING("{\"text\":\"\",\"source\":\"none\",\"sourceAge\":0}",
                           json.c_str());
}

// #403: the push carries the producer so the console can name it the moment
// the flaps turn, without chasing a /settings read behind every event.
static void test_payload_names_the_producer_and_its_age() {
  String json = buildDisplayEventJson("X", DisplaySource::Leader, 47);
  TEST_ASSERT_EQUAL_STRING(
      "{\"text\":\"X\",\"source\":\"leader\",\"sourceAge\":47}", json.c_str());
}

// --- wall mirror rows (#277) — leader-only payload extension ---------------------

static void test_rows_payload_shape() {
  String rows[2] = {String("AB"), String("C\"D")};
  String json = buildDisplayEventJson("X", DisplaySource::Clock, 3, rows, 2, 1);
  TEST_ASSERT_EQUAL_STRING(
      "{\"text\":\"X\",\"source\":\"clock\",\"sourceAge\":3,\"selfRow\":1,"
      "\"rows\":[\"AB\",\"C\\\"D\"]}",
      json.c_str());
}

static void test_zero_rows_keeps_plain_payload() {
  String json = buildDisplayEventJson("X", DisplaySource::Clock, 3, nullptr, 0, 0);
  TEST_ASSERT_EQUAL_STRING(
      "{\"text\":\"X\",\"source\":\"clock\",\"sourceAge\":3}", json.c_str());
}

static void test_row_change_alone_is_due() {
  DisplayEventTracker t;
  String rows[2] = {String("A"), String("B")};
  TEST_ASSERT_TRUE(displayEventDue(t, "X", displayEventRowsKey(rows, 2)));
  TEST_ASSERT_FALSE(displayEventDue(t, "X", displayEventRowsKey(rows, 2)));
  rows[1] = "CHANGED";  // a follower-only row changed; own text did not
  TEST_ASSERT_TRUE(displayEventDue(t, "X", displayEventRowsKey(rows, 2)));
}

static void test_leaving_cluster_mode_is_due() {
  DisplayEventTracker t;
  String rows[1] = {String("A")};
  TEST_ASSERT_TRUE(displayEventDue(t, "X", displayEventRowsKey(rows, 1)));
  // Cluster disabled: same text, empty rows key — still a push (the
  // browser must collapse back to the single-row mirror).
  TEST_ASSERT_TRUE(displayEventDue(t, "X", String()));
  TEST_ASSERT_FALSE(displayEventDue(t, "X"));  // plain API sees it settled
}

static void test_rows_key_separates_row_boundaries() {
  String ab[2] = {String("A"), String("B")};
  String a_b[1] = {String("A\nB")};
  TEST_ASSERT_TRUE(displayEventRowsKey(ab, 2) != displayEventRowsKey(a_b, 1));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_first_nonempty_text_is_due_once);
  RUN_TEST(test_empty_initial_text_is_not_due);
  RUN_TEST(test_change_and_stop_clear_are_due);
  RUN_TEST(test_overlong_text_is_truncated_not_overflowed);
  RUN_TEST(test_payload_shape_and_escaping);
  RUN_TEST(test_payload_empty_text);
  RUN_TEST(test_payload_names_the_producer_and_its_age);
  RUN_TEST(test_rows_payload_shape);
  RUN_TEST(test_zero_rows_keeps_plain_payload);
  RUN_TEST(test_row_change_alone_is_due);
  RUN_TEST(test_leaving_cluster_mode_is_due);
  RUN_TEST(test_rows_key_separates_row_boundaries);
  UNITY_END();
  return 0;
}
