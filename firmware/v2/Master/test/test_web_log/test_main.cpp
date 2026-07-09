// Host-side tests for the v2 web log ring buffer (#186, #187).
// The ring is byte-oriented: readers get the last WEBLOG_SIZE bytes of
// serial-style output, newlines intact, oldest first. A tiny ring size is
// injected here so wrap behaviour is cheap to exercise.
//
// Since #187 the buffer is allocated by webLogInit() via largeAlloc()
// (PSRAM-preferred on the S3) instead of living in static SRAM; anything
// logged before init is dropped rather than crashing.

#include <ArduinoFake.h>
#include <unity.h>

#define WEBLOG_SIZE 16
#include "../../WebLog.cpp"

// setUp deliberately does NOT init: the first test in main() exercises the
// pre-init guards, everything after runs against an initialized ring
// (webLogInit() is idempotent, so re-running it in setUp would be fine but
// would make the pre-init state untestable).
void setUp() { webLogReset(); }
void tearDown() {}

// MUST run first (before any webLogInit): logging before init is dropped,
// not a crash — SerialPrint helpers may fire before setup() reaches init.
static void test_append_and_read_before_init_are_safe() {
  webLogAppend("early", 5);
  TEST_ASSERT_EQUAL_STRING("", webLogRead().c_str());
}

static void test_init_allocates_ring_and_is_idempotent() {
  webLogInit();
  webLogAppend("abc", 3);
  webLogInit();  // second init must not clobber or leak the ring
  TEST_ASSERT_EQUAL_STRING("abc", webLogRead().c_str());
}

static void test_empty_log_reads_empty() {
  TEST_ASSERT_EQUAL_STRING("", webLogRead().c_str());
}

static void test_short_append_round_trips() {
  webLogAppend("hello", 5);
  TEST_ASSERT_EQUAL_STRING("hello", webLogRead().c_str());
}

static void test_null_and_zero_length_appends_are_ignored() {
  webLogAppend(nullptr, 3);
  webLogAppend("x", 0);
  TEST_ASSERT_EQUAL_STRING("", webLogRead().c_str());
}

static void test_wrap_keeps_last_ring_size_bytes_in_order() {
  // 20 bytes into a 16-byte ring: the first 4 fall off the front.
  webLogAppend("abcdefghijklmnopqrst", 20);
  TEST_ASSERT_EQUAL_STRING("efghijklmnopqrst", webLogRead().c_str());
}

static void test_exact_fill_then_one_more_drops_oldest_byte() {
  webLogAppend("0123456789abcdef", 16);
  TEST_ASSERT_EQUAL_STRING("0123456789abcdef", webLogRead().c_str());
  webLogAppend("X", 1);
  TEST_ASSERT_EQUAL_STRING("123456789abcdefX", webLogRead().c_str());
}

// Only the write() overrides are ours — Print's formatting helpers are the
// Arduino core's (and fakeit-mocked under ArduinoFake, so not callable here).
static void test_printer_single_byte_write_lands_in_ring() {
  TEST_ASSERT_EQUAL(1, webLogPrinter.write((uint8_t)'x'));
  TEST_ASSERT_EQUAL_STRING("x", webLogRead().c_str());
}

static void test_printer_buffer_write_lands_in_ring() {
  TEST_ASSERT_EQUAL(5, webLogPrinter.write((const uint8_t*)"hello", 5));
  TEST_ASSERT_EQUAL_STRING("hello", webLogRead().c_str());
}

static void test_reset_clears_wrapped_state() {
  webLogAppend("abcdefghijklmnopqrst", 20);  // wrapped
  webLogReset();
  TEST_ASSERT_EQUAL_STRING("", webLogRead().c_str());
  webLogAppend("fresh", 5);
  TEST_ASSERT_EQUAL_STRING("fresh", webLogRead().c_str());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_append_and_read_before_init_are_safe);  // pre-init, keep first
  RUN_TEST(test_init_allocates_ring_and_is_idempotent);
  RUN_TEST(test_empty_log_reads_empty);
  RUN_TEST(test_short_append_round_trips);
  RUN_TEST(test_null_and_zero_length_appends_are_ignored);
  RUN_TEST(test_wrap_keeps_last_ring_size_bytes_in_order);
  RUN_TEST(test_exact_fill_then_one_more_drops_oldest_byte);
  RUN_TEST(test_printer_single_byte_write_lands_in_ring);
  RUN_TEST(test_printer_buffer_write_lands_in_ring);
  RUN_TEST(test_reset_clears_wrapped_state);
  return UNITY_END();
}
