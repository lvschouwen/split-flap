// Native tests for EventRecordPolicy.h: one entry, the wait for the clock,
// and reading the record back newest first.
#include <unity.h>

#include <vector>

#include "../../EventRecordPolicy.h"

void setUp() {}
void tearDown() {}

struct MemFile {
  std::vector<uint8_t> bytes;
  int reads = 0;
  size_t size() { return bytes.size(); }
  bool read(size_t offset, uint8_t* buf, size_t n) {
    if (offset + n > bytes.size()) return false;
    memcpy(buf, bytes.data() + offset, n);
    reads++;
    return true;
  }
  void append(const EventRecord& r) {
    uint8_t raw[EVENT_RECORD_SIZE];
    eventEncode(r, raw);
    bytes.insert(bytes.end(), raw, raw + EVENT_RECORD_SIZE);
  }
  void appendSeqs(uint32_t from, uint32_t to) {
    for (uint32_t s = from; s <= to; s++) {
      EventRecord r;
      r.seq = s;
      r.kind = (uint8_t)EventKind::JobDone;
      append(r);
    }
  }
  // A write cut short after `n` bytes, then put back on the boundary.
  void tear(size_t n) {
    bytes.insert(bytes.end(), n, 0xEE);
    bytes.insert(bytes.end(), eventPadNeeded(bytes.size()), 0);
  }
};

static EventRecord sample() {
  EventRecord r;
  r.timeS = 1791230000UL;
  r.seq = 0x01020304;
  r.a = 0xA1A2A3A4;
  r.b = 0xB1B2B3B4;
  r.board = 0xC1C2;
  r.kind = (uint8_t)EventKind::UnitReasonOn;
  r.detail = 7;
  r.unit = 9;
  return r;
}

// The bytes are the contract with every record already on a wall.
static void test_an_entry_has_this_layout() {
  uint8_t raw[EVENT_RECORD_SIZE];
  eventEncode(sample(), raw);
  const uint8_t want[EVENT_RECORD_SIZE] = {
      0x30, 0x00, 0xC4, 0x6A, 0x04, 0x03, 0x02, 0x01, 0xA4, 0xA3, 0xA2, 0xA1,
      0xB4, 0xB3, 0xB2, 0xB1, 0xC2, 0xC1, 10,   7,    9,    raw[21], 0,   0};
  TEST_ASSERT_EQUAL_HEX8_ARRAY(want, raw, EVENT_RECORD_SIZE);
  uint8_t x = EVENT_CHECK_MASK;
  for (int i = 0; i < EVENT_RECORD_SIZE; i++) {
    if (i != 21) x ^= raw[i];
  }
  TEST_ASSERT_EQUAL_HEX8(x, raw[21]);
}

static void test_an_entry_reads_back() {
  uint8_t raw[EVENT_RECORD_SIZE];
  eventEncode(sample(), raw);
  EventRecord r;
  TEST_ASSERT_TRUE(eventDecode(raw, r));
  TEST_ASSERT_EQUAL_UINT32(1791230000UL, r.timeS);
  TEST_ASSERT_EQUAL_HEX32(0x01020304, r.seq);
  TEST_ASSERT_EQUAL_HEX32(0xA1A2A3A4, r.a);
  TEST_ASSERT_EQUAL_HEX32(0xB1B2B3B4, r.b);
  TEST_ASSERT_EQUAL_HEX16(0xC1C2, r.board);
  TEST_ASSERT_EQUAL_UINT8(10, r.kind);
  TEST_ASSERT_EQUAL_UINT8(7, r.detail);
  TEST_ASSERT_EQUAL_UINT8(9, r.unit);
}

static void test_damaged_and_empty_bytes_are_no_entry() {
  uint8_t raw[EVENT_RECORD_SIZE];
  EventRecord r;
  for (int i = 0; i < EVENT_RECORD_SIZE; i++) {
    eventEncode(sample(), raw);
    raw[i] ^= 0x10;
    TEST_ASSERT_FALSE(eventDecode(raw, r));
  }
  memset(raw, 0x00, sizeof(raw));
  TEST_ASSERT_FALSE(eventDecode(raw, r));
  memset(raw, 0xFF, sizeof(raw));
  TEST_ASSERT_FALSE(eventDecode(raw, r));
  EventRecord zeroSeq = sample();
  zeroSeq.seq = 0;
  eventEncode(zeroSeq, raw);
  TEST_ASSERT_FALSE(eventDecode(raw, r));
}

static void test_the_kind_numbers_and_names_are_fixed() {
  static const struct { EventKind k; uint8_t n; const char* name; } fixed[] = {
      {EventKind::MasterStarted, 1, "master-started"},
      {EventKind::EventsDropped, 2, "events-dropped"},
      {EventKind::UnitReasonOn, 10, "unit-reason-on"},
      {EventKind::UnitReasonOff, 11, "unit-reason-off"},
      {EventKind::UnitRestarted, 12, "unit-restarted"},
      {EventKind::BoardReasonOn, 20, "board-reason-on"},
      {EventKind::BoardReasonOff, 21, "board-reason-off"},
      {EventKind::RowStarted, 22, "row-started"},
      {EventKind::JobDone, 30, "job-done"},
      {EventKind::JobFailed, 31, "job-failed"},
      {EventKind::RowEvent, 40, "row-event"},
  };
  int named = 0;
  for (int k = 0; k < 256; k++) {
    if (strcmp(eventKindName((uint8_t)k), "?") != 0) named++;
  }
  TEST_ASSERT_EQUAL((int)(sizeof(fixed) / sizeof(fixed[0])), named);
  for (const auto& f : fixed) {
    TEST_ASSERT_EQUAL_UINT8(f.n, (uint8_t)f.k);
    TEST_ASSERT_EQUAL_STRING(f.name, eventKindName(f.n));
  }
}

static void test_a_board_key_names_the_master_zero_and_no_other_board() {
  TEST_ASSERT_EQUAL_UINT16(0, eventBoardKey(""));
  TEST_ASSERT_EQUAL_UINT16(0, eventBoardKey(nullptr));
  // Pinned: the key of a board is in every entry about it.
  TEST_ASSERT_EQUAL_HEX16(0x438E, eventBoardKey("split-flap-261bb6"));
  TEST_ASSERT_NOT_EQUAL(eventBoardKey("split-flap-261bb6"), eventBoardKey("split-flap-261bb7"));
  TEST_ASSERT_NOT_EQUAL(0, eventBoardKey("a"));
}

static void test_a_rev_as_a_number() {
  TEST_ASSERT_EQUAL_HEX32(0x97E2679, eventRevNumber("97e2679"));
  TEST_ASSERT_EQUAL_HEX32(0x97E2679, eventRevNumber("97e2679-dirty"));
  TEST_ASSERT_EQUAL_HEX32(0x12345678, eventRevNumber("123456789abc"));
  TEST_ASSERT_EQUAL_HEX32(0xABC, eventRevNumber("ABC"));
  TEST_ASSERT_EQUAL_HEX32(0, eventRevNumber("unknown"));
  TEST_ASSERT_EQUAL_HEX32(0, eventRevNumber(nullptr));
}

// ---- the wait for the clock --------------------------------------------------

static EventRecord kind(EventKind k) {
  EventRecord r;
  r.kind = (uint8_t)k;
  return r;
}

static void test_with_the_clock_set_entries_are_written_at_once_with_their_time() {
  EventStage stage;
  stage.put(kind(EventKind::JobDone), 10000);
  stage.put(kind(EventKind::JobFailed), 14000);
  EventRecord out[EVENT_STAGE_CAP + 1];
  uint32_t seq = 7;
  const int n = stage.take(1791230000UL, true, 15000, false, seq, out, EVENT_STAGE_CAP + 1);
  TEST_ASSERT_EQUAL(2, n);
  TEST_ASSERT_EQUAL_UINT32(1791229995UL, out[0].timeS);
  TEST_ASSERT_EQUAL_UINT32(1791229999UL, out[1].timeS);
  TEST_ASSERT_EQUAL_UINT32(7, out[0].seq);
  TEST_ASSERT_EQUAL_UINT32(8, out[1].seq);
  TEST_ASSERT_EQUAL_UINT32(9, seq);
  TEST_ASSERT_EQUAL_UINT8(0, stage.count);
}

static void test_an_entry_from_before_the_clock_waits_for_it_and_is_dated_back() {
  EventStage stage;
  stage.put(kind(EventKind::MasterStarted), 500);
  EventRecord out[4];
  uint32_t seq = 1;
  TEST_ASSERT_EQUAL(0, stage.take(40, false, 30000, false, seq, out, 4));
  TEST_ASSERT_EQUAL_UINT8(1, stage.count);
  TEST_ASSERT_EQUAL(1, stage.take(1791230000UL, true, 30500, false, seq, out, 4));
  TEST_ASSERT_EQUAL_UINT32(1791229970UL, out[0].timeS);
}

static void test_without_a_clock_an_entry_is_written_undated_after_the_wait() {
  EventStage stage;
  stage.put(kind(EventKind::MasterStarted), 1000);
  stage.put(kind(EventKind::JobDone), 60000);
  EventRecord out[4];
  uint32_t seq = 1;
  TEST_ASSERT_EQUAL(0, stage.take(100, false, 1000 + EVENT_CLOCK_WAIT_MS - 1, false, seq, out, 4));
  // The older one is due, the younger one keeps waiting: order is kept.
  TEST_ASSERT_EQUAL(1, stage.take(100, false, 1000 + EVENT_CLOCK_WAIT_MS, false, seq, out, 4));
  TEST_ASSERT_EQUAL_UINT8((uint8_t)EventKind::MasterStarted, out[0].kind);
  TEST_ASSERT_EQUAL_UINT32(0, out[0].timeS);
  TEST_ASSERT_EQUAL_UINT8(1, stage.count);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)EventKind::JobDone, stage.q[0].record.kind);
}

static void test_before_a_restart_everything_is_written() {
  EventStage stage;
  stage.put(kind(EventKind::JobDone), 1000);
  EventRecord out[4];
  uint32_t seq = 1;
  TEST_ASSERT_EQUAL(1, stage.take(5, false, 2000, true, seq, out, 4));
  TEST_ASSERT_EQUAL_UINT32(0, out[0].timeS);
}

static void test_the_uptime_clock_may_wrap_while_an_entry_waits() {
  EventStage stage;
  stage.put(kind(EventKind::JobDone), 0xFFFFF000UL);
  EventRecord out[4];
  uint32_t seq = 1;
  TEST_ASSERT_EQUAL(1, stage.take(1791230000UL, true, 0x00000F00UL, false, seq, out, 4));
  TEST_ASSERT_EQUAL_UINT32(1791229993UL, out[0].timeS);  // 7936 ms earlier
}

static void test_entries_without_room_are_counted_and_owned_up_to() {
  EventStage stage;
  for (int i = 0; i < EVENT_STAGE_CAP; i++) TEST_ASSERT_TRUE(stage.put(kind(EventKind::JobDone), 100));
  TEST_ASSERT_FALSE(stage.put(kind(EventKind::JobDone), 100));
  TEST_ASSERT_FALSE(stage.put(kind(EventKind::JobDone), 100));
  EventRecord out[EVENT_STAGE_CAP + 1];
  uint32_t seq = 1;
  const int n = stage.take(1791230000UL, true, 200, false, seq, out, EVENT_STAGE_CAP + 1);
  TEST_ASSERT_EQUAL(EVENT_STAGE_CAP + 1, n);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)EventKind::EventsDropped, out[EVENT_STAGE_CAP].kind);
  TEST_ASSERT_EQUAL_UINT32(2, out[EVENT_STAGE_CAP].a);
  TEST_ASSERT_EQUAL_UINT32(EVENT_STAGE_CAP + 1, out[EVENT_STAGE_CAP].seq);
  TEST_ASSERT_EQUAL_UINT32(0, stage.dropped);
  TEST_ASSERT_EQUAL(0, stage.take(1791230000UL, true, 300, false, seq, out, EVENT_STAGE_CAP + 1));
}

static void test_the_count_of_lost_entries_waits_for_room_to_be_written() {
  EventStage stage;
  for (int i = 0; i < EVENT_STAGE_CAP + 3; i++) stage.put(kind(EventKind::JobDone), 100);
  EventRecord out[EVENT_STAGE_CAP];
  uint32_t seq = 1;
  TEST_ASSERT_EQUAL(EVENT_STAGE_CAP, stage.take(1791230000UL, true, 200, false, seq, out, EVENT_STAGE_CAP));
  TEST_ASSERT_EQUAL_UINT32(3, stage.dropped);
  TEST_ASSERT_EQUAL(1, stage.take(1791230000UL, true, 300, false, seq, out, EVENT_STAGE_CAP));
  TEST_ASSERT_EQUAL_UINT32(3, out[0].a);
}

// ---- the files ---------------------------------------------------------------

static void test_padding_and_rotation() {
  TEST_ASSERT_EQUAL(0, eventPadNeeded(0));
  TEST_ASSERT_EQUAL(0, eventPadNeeded(48));
  TEST_ASSERT_EQUAL(23, eventPadNeeded(49));
  TEST_ASSERT_EQUAL(1, eventPadNeeded(71));
  TEST_ASSERT_FALSE(eventShouldRotate((size_t)EVENT_FILE_RECORDS * EVENT_RECORD_SIZE - 1));
  TEST_ASSERT_TRUE(eventShouldRotate((size_t)EVENT_FILE_RECORDS * EVENT_RECORD_SIZE));
}

static void test_the_newest_seq_of_a_file() {
  MemFile f;
  TEST_ASSERT_EQUAL_UINT32(0, eventLastSeq(f));
  f.appendSeqs(5, 9);
  TEST_ASSERT_EQUAL_UINT32(9, eventLastSeq(f));
  f.tear(10);
  TEST_ASSERT_EQUAL_UINT32(9, eventLastSeq(f));
  f.bytes.push_back(0x01);  // an unfinished entry at the very end
  TEST_ASSERT_EQUAL_UINT32(9, eventLastSeq(f));
}

static void test_a_page_is_the_newest_entries_first() {
  MemFile f;
  f.appendSeqs(1, 40);
  EventRecord out[10];
  TEST_ASSERT_EQUAL(10, eventPageRead(f, 0, out, 10, 0));
  TEST_ASSERT_EQUAL_UINT32(40, out[0].seq);
  TEST_ASSERT_EQUAL_UINT32(31, out[9].seq);
}

static void test_the_next_page_starts_below_the_last() {
  MemFile f;
  f.appendSeqs(1, 40);
  EventRecord out[10];
  TEST_ASSERT_EQUAL(10, eventPageRead(f, 31, out, 10, 0));
  TEST_ASSERT_EQUAL_UINT32(30, out[0].seq);
  TEST_ASSERT_EQUAL_UINT32(21, out[9].seq);
  TEST_ASSERT_EQUAL(5, eventPageRead(f, 6, out, 10, 0));
  TEST_ASSERT_EQUAL_UINT32(5, out[0].seq);
  TEST_ASSERT_EQUAL_UINT32(1, out[4].seq);
  TEST_ASSERT_EQUAL(0, eventPageRead(f, 1, out, 10, 0));
}

static void test_a_page_far_back_is_reached_without_reading_the_whole_file() {
  MemFile f;
  f.appendSeqs(1, 2000);
  EventRecord out[10];
  f.reads = 0;
  TEST_ASSERT_EQUAL(10, eventPageRead(f, 101, out, 10, 0));
  TEST_ASSERT_EQUAL_UINT32(100, out[0].seq);
  TEST_ASSERT_LESS_THAN(5, f.reads);
}

static void test_a_page_continues_into_the_older_file() {
  MemFile current, previous;
  previous.appendSeqs(1, 20);
  current.appendSeqs(21, 26);
  EventRecord out[10];
  int n = eventPageRead(current, 0, out, 10, 0);
  TEST_ASSERT_EQUAL(6, n);
  n = eventPageRead(previous, 0, out, 10, n);
  TEST_ASSERT_EQUAL(10, n);
  TEST_ASSERT_EQUAL_UINT32(26, out[0].seq);
  TEST_ASSERT_EQUAL_UINT32(21, out[5].seq);
  TEST_ASSERT_EQUAL_UINT32(20, out[6].seq);
  TEST_ASSERT_EQUAL_UINT32(17, out[9].seq);
  // Asked for what lies wholly in the older file, the newer one gives nothing.
  TEST_ASSERT_EQUAL(0, eventPageRead(current, 15, out, 10, 0));
  TEST_ASSERT_EQUAL(10, eventPageRead(previous, 15, out, 10, 0));
  TEST_ASSERT_EQUAL_UINT32(14, out[0].seq);
}

static void test_a_torn_write_costs_one_entry_and_no_other() {
  MemFile f;
  f.appendSeqs(1, 10);
  f.tear(7);
  f.appendSeqs(11, 20);
  f.tear(30);  // more than one entry's worth of rubbish
  f.appendSeqs(21, 25);
  EventRecord out[30];
  TEST_ASSERT_EQUAL(25, eventPageRead(f, 0, out, 30, 0));
  for (int i = 0; i < 25; i++) TEST_ASSERT_EQUAL_UINT32((uint32_t)(25 - i), out[i].seq);
  // Paging across the damage still finds every entry once.
  TEST_ASSERT_EQUAL(4, eventPageRead(f, 13, out, 4, 0));
  TEST_ASSERT_EQUAL_UINT32(12, out[0].seq);
  TEST_ASSERT_EQUAL_UINT32(9, out[3].seq);
  TEST_ASSERT_EQUAL(8, eventPageRead(f, 9, out, 30, 0));
  TEST_ASSERT_EQUAL_UINT32(8, out[0].seq);
}

static void test_an_empty_or_rubbish_file_gives_nothing() {
  MemFile f;
  EventRecord out[4];
  TEST_ASSERT_EQUAL(0, eventPageRead(f, 0, out, 4, 0));
  f.bytes.assign(100, 0xEE);
  TEST_ASSERT_EQUAL(0, eventPageRead(f, 0, out, 4, 0));
  TEST_ASSERT_EQUAL(0, eventPageRead(f, 50, out, 4, 0));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_an_entry_has_this_layout);
  RUN_TEST(test_an_entry_reads_back);
  RUN_TEST(test_damaged_and_empty_bytes_are_no_entry);
  RUN_TEST(test_the_kind_numbers_and_names_are_fixed);
  RUN_TEST(test_a_board_key_names_the_master_zero_and_no_other_board);
  RUN_TEST(test_a_rev_as_a_number);
  RUN_TEST(test_with_the_clock_set_entries_are_written_at_once_with_their_time);
  RUN_TEST(test_an_entry_from_before_the_clock_waits_for_it_and_is_dated_back);
  RUN_TEST(test_without_a_clock_an_entry_is_written_undated_after_the_wait);
  RUN_TEST(test_before_a_restart_everything_is_written);
  RUN_TEST(test_the_uptime_clock_may_wrap_while_an_entry_waits);
  RUN_TEST(test_entries_without_room_are_counted_and_owned_up_to);
  RUN_TEST(test_the_count_of_lost_entries_waits_for_room_to_be_written);
  RUN_TEST(test_padding_and_rotation);
  RUN_TEST(test_the_newest_seq_of_a_file);
  RUN_TEST(test_a_page_is_the_newest_entries_first);
  RUN_TEST(test_the_next_page_starts_below_the_last);
  RUN_TEST(test_a_page_far_back_is_reached_without_reading_the_whole_file);
  RUN_TEST(test_a_page_continues_into_the_older_file);
  RUN_TEST(test_a_torn_write_costs_one_entry_and_no_other);
  RUN_TEST(test_an_empty_or_rubbish_file_gives_nothing);
  return UNITY_END();
}
