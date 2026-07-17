// Host-side tests for the log-line timestamp prefixer (#318 E). Pure logic:
// given a caller-supplied stamp and a stream of byte chunks (as the web/flash
// log tee delivers them), each real line gets "<stamp> " once, blank lines
// stay clean, and the line-start state survives across chunk boundaries.

#include <ArduinoFake.h>
#include <unity.h>

#include "../../LogLinePrefixer.h"

void setUp() {}
void tearDown() {}

static String run(LogLinePrefixer& p, const char* stamp, const char* data) {
  String out;
  p.expand(stamp, data, strlen(data), out);
  return out;
}

static void test_single_line_gets_one_stamp() {
  LogLinePrefixer p;
  TEST_ASSERT_EQUAL_STRING("12:00:00 hello\n",
                           run(p, "12:00:00", "hello\n").c_str());
}

static void test_multiple_lines_each_stamped() {
  LogLinePrefixer p;
  TEST_ASSERT_EQUAL_STRING("12:00:00 a\n12:00:00 b\n",
                           run(p, "12:00:00", "a\nb\n").c_str());
}

// SerialPrintln writes the body and the "\r\n" as separate appends: the
// stamp must land once, on the body chunk, not again on the newline chunk.
static void test_line_start_state_survives_across_chunks() {
  LogLinePrefixer p;
  String out;
  p.expand("09:00:00", "body", 4, out);
  p.expand("09:00:00", "\r\n", 2, out);
  TEST_ASSERT_EQUAL_STRING("09:00:00 body\r\n", out.c_str());
}

static void test_blank_lines_are_not_stamped() {
  LogLinePrefixer p;
  // A spacer line (just a newline) stays blank; the following real line is
  // stamped.
  TEST_ASSERT_EQUAL_STRING("\n12:00:00 real\n",
                           run(p, "12:00:00", "\nreal\n").c_str());
}

static void test_crlf_blank_line_not_stamped() {
  LogLinePrefixer p;
  TEST_ASSERT_EQUAL_STRING("\r\n12:00:00 x\n",
                           run(p, "12:00:00", "\r\nx\n").c_str());
}

// Trailing content with no newline yet: stamped, and the NEXT chunk must not
// re-stamp because the line is still open.
static void test_open_line_not_restamped_on_continuation() {
  LogLinePrefixer p;
  String out;
  p.expand("01:01:01", "par", 3, out);
  p.expand("01:01:01", "tial\n", 5, out);
  TEST_ASSERT_EQUAL_STRING("01:01:01 partial\n", out.c_str());
}

// Empty stamp is the native escape hatch: verbatim passthrough, no prefix.
static void test_empty_stamp_is_verbatim() {
  LogLinePrefixer p;
  TEST_ASSERT_EQUAL_STRING("a\nb\n", run(p, "", "a\nb\n").c_str());
}

static void test_null_stamp_is_verbatim() {
  LogLinePrefixer p;
  TEST_ASSERT_EQUAL_STRING("a\nb\n", run(p, nullptr, "a\nb\n").c_str());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_single_line_gets_one_stamp);
  RUN_TEST(test_multiple_lines_each_stamped);
  RUN_TEST(test_line_start_state_survives_across_chunks);
  RUN_TEST(test_blank_lines_are_not_stamped);
  RUN_TEST(test_crlf_blank_line_not_stamped);
  RUN_TEST(test_open_line_not_restamped_on_continuation);
  RUN_TEST(test_empty_stamp_is_verbatim);
  RUN_TEST(test_null_stamp_is_verbatim);
  return UNITY_END();
}
