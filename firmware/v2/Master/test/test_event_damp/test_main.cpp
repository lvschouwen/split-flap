// Native tests for EventDampPolicy.h: the same entry is written ten times an
// hour, then counted, and the count is written once the hour has passed.
#include <unity.h>

#include "../../EventDampPolicy.h"

void setUp() {}
void tearDown() {}

static const uint32_t T0 = 1792440000UL;

static EventRecord started(uint32_t timeS, uint32_t rev = 0x29b1fc3) {
  EventRecord r;
  r.kind = (uint8_t)EventKind::MasterStarted;
  r.detail = 3;
  r.a = rev;
  r.timeS = timeS;
  return r;
}

static EventDamp fresh() {
  EventDamp d;
  d.clear();
  return d;
}

static int admitAll(EventDamp& d, const EventRecord& r, int times, uint32_t stepS) {
  int written = 0;
  for (int i = 0; i < times; i++) {
    EventRecord e = r;
    e.timeS = r.timeS + (uint32_t)i * stepS;
    EventRecord summary;
    bool hasSummary;
    if (d.admit(e, summary, hasSummary)) written++;
  }
  return written;
}

static void test_ten_identical_entries_an_hour_are_written_and_the_rest_counted() {
  EventDamp d = fresh();
  TEST_ASSERT_EQUAL(EVENT_DAMP_BURST, admitAll(d, started(T0), 40, 30));
  EventRecord out[4];
  TEST_ASSERT_EQUAL(0, d.due(T0 + EVENT_DAMP_WINDOW_S - 1, out, 4));
  TEST_ASSERT_EQUAL(1, d.due(T0 + EVENT_DAMP_WINDOW_S, out, 4));
  TEST_ASSERT_EQUAL_UINT8((uint8_t)EventKind::Repeated, out[0].kind);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)EventKind::MasterStarted, out[0].detail);
  TEST_ASSERT_EQUAL_UINT32(30, out[0].a);
  TEST_ASSERT_EQUAL_UINT32(3, out[0].b);
  TEST_ASSERT_EQUAL_UINT32(T0 + 39 * 30, out[0].timeS);
  // The slot is free again: the next one opens a new window.
  TEST_ASSERT_EQUAL(0, d.due(T0 + 2 * EVENT_DAMP_WINDOW_S, out, 4));
  TEST_ASSERT_EQUAL(1, admitAll(d, started(T0 + 2 * EVENT_DAMP_WINDOW_S), 1, 0));
}

static void test_entries_that_differ_in_any_field_are_not_damped() {
  EventDamp d = fresh();
  for (uint32_t i = 0; i < 30; i++) {
    TEST_ASSERT_EQUAL(1, admitAll(d, started(T0 + i, 0x1000 + i), 1, 0));
  }
  EventRecord other = started(T0);
  other.unit = 5;
  admitAll(d, started(T0), EVENT_DAMP_BURST, 1);
  TEST_ASSERT_EQUAL(1, admitAll(d, other, 1, 0));
}

static void test_a_window_that_starts_over_says_what_the_last_one_did_not_write() {
  EventDamp d = fresh();
  admitAll(d, started(T0), 12, 60);
  EventRecord summary;
  bool hasSummary = false;
  TEST_ASSERT_TRUE(d.admit(started(T0 + EVENT_DAMP_WINDOW_S + 5), summary, hasSummary));
  TEST_ASSERT_TRUE(hasSummary);
  TEST_ASSERT_EQUAL_UINT32(2, summary.a);
  TEST_ASSERT_EQUAL_UINT32(T0 + 11 * 60, summary.timeS);
  // A window that wrote all it was given owes no summary.
  EventDamp quiet = fresh();
  admitAll(quiet, started(T0), 3, 60);
  TEST_ASSERT_TRUE(quiet.admit(started(T0 + EVENT_DAMP_WINDOW_S), summary, hasSummary));
  TEST_ASSERT_FALSE(hasSummary);
}

static void test_an_entry_without_a_time_is_always_written() {
  EventDamp d = fresh();
  TEST_ASSERT_EQUAL(40, admitAll(d, started(0), 40, 0));
}

static void test_a_full_table_gives_up_its_oldest_window_with_its_count() {
  EventDamp d = fresh();
  admitAll(d, started(T0), 15, 1);  // 5 not written
  for (uint32_t i = 1; i < EVENT_DAMP_SLOTS; i++) admitAll(d, started(T0 + 100 + i, i), 1, 0);
  EventRecord summary;
  bool hasSummary = false;
  TEST_ASSERT_TRUE(d.admit(started(T0 + 200, 0xBEEF), summary, hasSummary));
  TEST_ASSERT_TRUE(hasSummary);
  TEST_ASSERT_EQUAL_UINT32(5, summary.a);
  TEST_ASSERT_EQUAL_UINT32(3, summary.b);
}

static void test_entries_read_back_after_a_power_cut_count_as_written() {
  EventDamp d = fresh();
  for (int i = 0; i < 30; i++) d.seed(started(T0 + (uint32_t)i * 30));
  EventRecord out[2];
  TEST_ASSERT_EQUAL(0, d.due(T0 + EVENT_DAMP_WINDOW_S, out, 2));
  EventDamp e = fresh();
  for (int i = 0; i < EVENT_DAMP_BURST; i++) e.seed(started(T0 + (uint32_t)i));
  TEST_ASSERT_EQUAL(0, admitAll(e, started(T0 + 100), 1, 0));
}

static void test_the_table_knows_when_it_is_not_its_own() {
  EventDamp d = fresh();
  TEST_ASSERT_TRUE(d.valid());
  admitAll(d, started(T0), 3, 1);
  TEST_ASSERT_TRUE(d.valid());
  d.slots[0].written ^= 1;
  TEST_ASSERT_FALSE(d.valid());
  EventDamp garbage;
  memset(&garbage, 0xA5, sizeof(garbage));
  TEST_ASSERT_FALSE(garbage.valid());
}

static void test_a_repeated_entry_is_never_damped_itself() {
  EventDamp d = fresh();
  EventRecord r;
  r.kind = (uint8_t)EventKind::Repeated;
  r.timeS = T0;
  TEST_ASSERT_EQUAL(30, admitAll(d, r, 30, 1));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_ten_identical_entries_an_hour_are_written_and_the_rest_counted);
  RUN_TEST(test_entries_that_differ_in_any_field_are_not_damped);
  RUN_TEST(test_a_window_that_starts_over_says_what_the_last_one_did_not_write);
  RUN_TEST(test_an_entry_without_a_time_is_always_written);
  RUN_TEST(test_a_full_table_gives_up_its_oldest_window_with_its_count);
  RUN_TEST(test_entries_read_back_after_a_power_cut_count_as_written);
  RUN_TEST(test_the_table_knows_when_it_is_not_its_own);
  RUN_TEST(test_a_repeated_entry_is_never_damped_itself);
  return UNITY_END();
}
