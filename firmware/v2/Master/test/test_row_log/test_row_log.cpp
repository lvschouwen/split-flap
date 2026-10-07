// Native tests for RowLogPolicy.h: a row board's log as the master keeps it.
#include <unity.h>

#include <string>

#include "RowLogPolicy.h"

static char store[64];
static RowLogRing ring;

void setUp() { ring.begin(store, sizeof(store)); }
void tearDown() {}

static std::string text() {
  char out[sizeof(store) + 1];
  const size_t n = ring.read(out, sizeof(out));
  return std::string(out, n);
}

static void add(const char* line) { ring.append((const uint8_t*)line, strlen(line)); }

static void test_lines_come_back_in_order_each_with_its_newline() {
  TEST_ASSERT_EQUAL_STRING("", text().c_str());
  add("[1] one");
  add("[2] two");
  TEST_ASSERT_EQUAL_STRING("[1] one\n[2] two\n", text().c_str());
}

static void test_a_full_ring_drops_whole_lines_from_its_old_end() {
  for (int i = 0; i < 9; i++) add("0123456789");  // 11 bytes a line, 64 fit five
  const std::string kept = text();
  TEST_ASSERT_EQUAL(55, kept.size());
  TEST_ASSERT_EQUAL_STRING("0123456789\n0123456789\n0123456789\n0123456789\n0123456789\n",
                           kept.c_str());
  add("new");
  TEST_ASSERT_TRUE(text().rfind("0123456789\nnew\n") != std::string::npos);
  TEST_ASSERT_EQUAL('0', text()[0]);  // never the tail of a cut line
}

static void test_a_line_longer_than_the_ring_keeps_its_end() {
  std::string line(100, 'x');
  line += "END";
  add(line.c_str());
  const std::string kept = text();
  TEST_ASSERT_EQUAL(sizeof(store), kept.size());
  TEST_ASSERT_TRUE(kept.rfind("END\n") == kept.size() - 4);
}

static void test_a_newline_inside_a_line_cannot_forge_a_second_line() {
  add("[3] a\n[9] forged");
  TEST_ASSERT_EQUAL_STRING("[3] a [9] forged\n", text().c_str());
}

static void test_a_short_reader_gets_the_newest_whole_lines() {
  add("[1] one");
  add("[2] two");
  char out[10];
  const size_t n = ring.read(out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("[2] two\n", std::string(out, n).c_str());
}

static void test_clearing_forgets_everything() {
  add("[1] one");
  ring.clear();
  TEST_ASSERT_EQUAL_STRING("", text().c_str());
  add("[2] two");
  TEST_ASSERT_EQUAL_STRING("[2] two\n", text().c_str());
}

static void test_the_log_is_asked_for_while_someone_read_it_lately() {
  RowLogWant want;
  TEST_ASSERT_FALSE(want.wanted(0));
  TEST_ASSERT_FALSE(want.wanted(5000));
  want.asked(1000);
  TEST_ASSERT_TRUE(want.wanted(1000));
  TEST_ASSERT_TRUE(want.wanted(1000 + ROW_LOG_HOLD_MS - 1));
  TEST_ASSERT_FALSE(want.wanted(1000 + ROW_LOG_HOLD_MS));
  // Asked a moment after the pass that judges it began.
  want.asked(2010);
  TEST_ASSERT_TRUE(want.wanted(2000));
  // Across the wrap of the clock.
  want.asked(0xFFFFFF00u);
  TEST_ASSERT_TRUE(want.wanted(0x00000100u));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_lines_come_back_in_order_each_with_its_newline);
  RUN_TEST(test_a_full_ring_drops_whole_lines_from_its_old_end);
  RUN_TEST(test_a_line_longer_than_the_ring_keeps_its_end);
  RUN_TEST(test_a_newline_inside_a_line_cannot_forge_a_second_line);
  RUN_TEST(test_a_short_reader_gets_the_newest_whole_lines);
  RUN_TEST(test_clearing_forgets_everything);
  RUN_TEST(test_the_log_is_asked_for_while_someone_read_it_lately);
  return UNITY_END();
}
