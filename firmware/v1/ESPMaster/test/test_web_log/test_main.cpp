// Host-side unit tests for the WebLog ring buffer (#170).
// ServiceWebLog.ino is textually included so the tests exercise the real
// code; its file-static ring state lands in this TU, letting setUp() reset
// the buffer between cases.

#include <ArduinoFake.h>
#include <unity.h>

#include <cstring>

// WebLog.h only defines WEBLOG_SIZE if unset — a tiny capacity here keeps
// the wrap cases explicit and hand-checkable.
#define WEBLOG_SIZE 16

#include "../../ServiceWebLog.ino"

using namespace fakeit;

void setUp() {
  ArduinoFakeReset();
  // noInterrupts()/interrupts() expand to the cli()/sei() fakeit mocks.
  When(Method(ArduinoFake(Function), cli)).AlwaysReturn();
  When(Method(ArduinoFake(Function), sei)).AlwaysReturn();
  memset(webLogBuffer, 0, sizeof(webLogBuffer));
  webLogHead = 0;
  webLogWrapped = false;
}
void tearDown() {}

static void appendStr(const char* s) {
  webLogAppend(s, strlen(s));
}

// --- pre-wrap behavior ---

static void test_empty_log_reads_empty() {
  TEST_ASSERT_EQUAL_STRING("", webLogRead().c_str());
}

static void test_fill_below_capacity_preserves_order() {
  appendStr("hello");
  TEST_ASSERT_EQUAL_STRING("hello", webLogRead().c_str());
}

static void test_multiple_appends_concatenate() {
  appendStr("abc");
  appendStr("de");
  appendStr("f");
  TEST_ASSERT_EQUAL_STRING("abcdef", webLogRead().c_str());
}

static void test_newlines_kept_intact() {
  appendStr("a\nb\n");
  TEST_ASSERT_EQUAL_STRING("a\nb\n", webLogRead().c_str());
}

// --- capacity boundary ---

// Exactly WEBLOG_SIZE bytes: head returns to 0 and the wrapped flag flips,
// but no data has been lost yet — the read must return all bytes in order.
static void test_exact_capacity_returns_everything_in_order() {
  appendStr("0123456789ABCDEF");  // 16 bytes == WEBLOG_SIZE
  TEST_ASSERT_EQUAL_STRING("0123456789ABCDEF", webLogRead().c_str());
}

static void test_one_past_capacity_drops_only_oldest_byte() {
  appendStr("0123456789ABCDEF");
  appendStr("G");
  TEST_ASSERT_EQUAL_STRING("123456789ABCDEFG", webLogRead().c_str());
}

// --- wrap-around ---

static void test_wrap_keeps_last_capacity_bytes() {
  appendStr("ABCDEFGHIJ");        // 10 bytes
  appendStr("KLMNOPQRST");        // 20 total: first 4 fall off
  TEST_ASSERT_EQUAL_STRING("EFGHIJKLMNOPQRST", webLogRead().c_str());
}

static void test_single_append_longer_than_capacity() {
  appendStr("0123456789ABCDEFGHIJ");  // 20 bytes in one call
  TEST_ASSERT_EQUAL_STRING("456789ABCDEFGHIJ", webLogRead().c_str());
}

static void test_multi_wrap_keeps_only_newest_tail() {
  for (int i = 0; i < 5; i++) appendStr("01234567");  // 40 bytes, 2.5 laps
  // 40 % 16 leaves head mid-buffer; the newest 16 bytes are two full chunks.
  TEST_ASSERT_EQUAL_STRING("0123456701234567", webLogRead().c_str());
}

// --- read semantics ---

static void test_read_does_not_consume() {
  appendStr("AAA");
  TEST_ASSERT_EQUAL_STRING("AAA", webLogRead().c_str());
  appendStr("BBB");
  TEST_ASSERT_EQUAL_STRING("AAABBB", webLogRead().c_str());
}

static void test_interleaved_append_read_after_wrap() {
  appendStr("0123456789ABCDEF");
  TEST_ASSERT_EQUAL_STRING("0123456789ABCDEF", webLogRead().c_str());
  appendStr("wxyz");
  TEST_ASSERT_EQUAL_STRING("456789ABCDEFwxyz", webLogRead().c_str());
  appendStr("!");
  TEST_ASSERT_EQUAL_STRING("56789ABCDEFwxyz!", webLogRead().c_str());
}

// --- input guards ---

static void test_null_and_zero_length_appends_are_ignored() {
  appendStr("keep");
  webLogAppend(nullptr, 5);
  webLogAppend("x", 0);
  TEST_ASSERT_EQUAL_STRING("keep", webLogRead().c_str());
  TEST_ASSERT_EQUAL_size_t(4, webLogHead);
  TEST_ASSERT_FALSE(webLogWrapped);
}

// --- Print sink ---

static void test_printer_single_byte_write() {
  TEST_ASSERT_EQUAL_size_t(1, webLogPrinter.write((uint8_t)'x'));
  TEST_ASSERT_EQUAL_STRING("x", webLogRead().c_str());
}

static void test_printer_buffer_write() {
  const char* msg = "log line";
  size_t written = webLogPrinter.write((const uint8_t*)msg, strlen(msg));
  TEST_ASSERT_EQUAL_size_t(strlen(msg), written);
  TEST_ASSERT_EQUAL_STRING("log line", webLogRead().c_str());
}

// Note: Print::print() itself is untestable here — ArduinoFake implements
// every print() overload as a per-instance fakeit mock that never routes
// through write(), so only the write() overrides (our code) are exercised.

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_empty_log_reads_empty);
  RUN_TEST(test_fill_below_capacity_preserves_order);
  RUN_TEST(test_multiple_appends_concatenate);
  RUN_TEST(test_newlines_kept_intact);
  RUN_TEST(test_exact_capacity_returns_everything_in_order);
  RUN_TEST(test_one_past_capacity_drops_only_oldest_byte);
  RUN_TEST(test_wrap_keeps_last_capacity_bytes);
  RUN_TEST(test_single_append_longer_than_capacity);
  RUN_TEST(test_multi_wrap_keeps_only_newest_tail);
  RUN_TEST(test_read_does_not_consume);
  RUN_TEST(test_interleaved_append_read_after_wrap);
  RUN_TEST(test_null_and_zero_length_appends_are_ignored);
  RUN_TEST(test_printer_single_byte_write);
  RUN_TEST(test_printer_buffer_write);
  return UNITY_END();
}
