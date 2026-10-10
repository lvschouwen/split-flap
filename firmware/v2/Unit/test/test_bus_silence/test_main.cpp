// Native tests for UnitBusSilence.h (when a unit that stops being addressed
// counts that as a silence, what it tries and what it writes down) and for the
// record's wire and EEPROM forms (shared/UnitBusRecord.h, UnitEeprom.h).
#include <unity.h>

#include <string.h>

#include <initializer_list>

#include "SplitFlapProtocol.h"

#include "../../UnitBusSilence.h"
#include "../../UnitEeprom.h"

void setUp() {}
void tearDown() {}

static const uint32_t MIN = 60000UL;
static const uint32_t AFTER = BUS_SILENCE_AFTER_MS;      // 3 minutes
static const uint32_t RESTART = BUS_SILENCE_RESTART_MS;  // 20 minutes

struct Unit {
  BusSilence s;
  UnitBusRecord r;
  uint16_t frames = 0;
  bool gate = false;
  uint8_t tick(uint32_t nowMs, bool lineLow = false, bool traffic = false) {
    return busSilenceTick(s, r, frames, lineLow, traffic, gate, nowMs);
  }
  // In regular contact up to `nowMs`.
  void contact(uint32_t nowMs) {
    frames += BUS_SILENCE_CONTACT_FRAMES;
    TEST_ASSERT_EQUAL_HEX8(0, tick(nowMs));
  }
};

static void test_the_spans_leave_room_for_what_a_master_does() {
  // One unit is asked every 3 s: the last of 16 waits 48 s between two.
  TEST_ASSERT_TRUE(AFTER >= 3 * 48000UL);
  // A whole row of units updated takes 16 minutes at the outside.
  TEST_ASSERT_TRUE(RESTART > 16 * MIN);
  TEST_ASSERT_TRUE(RESTART > 2 * AFTER);
}

static void test_a_unit_nobody_addressed_has_no_silence() {
  Unit u;
  for (uint32_t t = 0; t < 60 * MIN; t += 1000) TEST_ASSERT_EQUAL_HEX8(0, u.tick(t));
  TEST_ASSERT_EQUAL(0, u.r.silences);
  // A few frames (a scan that found it) are not regular contact yet.
  u.frames = BUS_SILENCE_CONTACT_FRAMES - 1;
  for (uint32_t t = 60 * MIN; t < 120 * MIN; t += 1000) TEST_ASSERT_EQUAL_HEX8(0, u.tick(t));
  TEST_ASSERT_EQUAL(0, u.r.silences);
}

static void test_a_span_without_a_frame_is_a_silence_and_restarts_the_bus_hardware() {
  Unit u;
  u.contact(5000);
  TEST_ASSERT_EQUAL_HEX8(0, u.tick(5000 + AFTER - 1));
  // Counted and acted on, not written yet.
  TEST_ASSERT_EQUAL_HEX8(BUS_SILENCE_DO_WATCH_TRAFFIC | BUS_SILENCE_DO_REINIT, u.tick(5000 + AFTER));
  TEST_ASSERT_EQUAL(1, u.r.silences);
  TEST_ASSERT_EQUAL(3, u.r.lastMinutes);
  TEST_ASSERT_EQUAL(3, u.r.longestMinutes);
  TEST_ASSERT_EQUAL(1, u.r.reinits);
  TEST_ASSERT_EQUAL_HEX8(0, u.r.flags);
  TEST_ASSERT_EQUAL(3, busSilenceNowMinutes(u.s, 5000 + AFTER));
  // Nothing more until the next span.
  TEST_ASSERT_EQUAL_HEX8(0, u.tick(5000 + AFTER + 1000));
}

static void test_the_bus_hardware_is_restarted_every_span_and_never_under_a_held_line() {
  Unit u;
  u.contact(0);
  u.tick(AFTER);
  TEST_ASSERT_EQUAL(1, u.r.reinits);
  // A held line is the other heal's.
  TEST_ASSERT_EQUAL_HEX8(BUS_SILENCE_DO_SAVE, u.tick(2 * AFTER, true));  // the first mark
  TEST_ASSERT_EQUAL(1, u.r.reinits);
  TEST_ASSERT_EQUAL_HEX8(BUS_SILENCE_DO_REINIT, u.tick(2 * AFTER + 10, false));
  TEST_ASSERT_EQUAL(2, u.r.reinits);
  TEST_ASSERT_EQUAL_HEX8(0, u.tick(3 * AFTER));
  TEST_ASSERT_EQUAL_HEX8(BUS_SILENCE_DO_REINIT, u.tick(3 * AFTER + 10));
}

static void test_a_line_held_low_at_the_start_is_noted() {
  Unit u;
  u.contact(0);
  TEST_ASSERT_EQUAL_HEX8(BUS_SILENCE_DO_WATCH_TRAFFIC, u.tick(AFTER, true));
  TEST_ASSERT_EQUAL_HEX8(BUS_RECORD_FLAG_LINE_LOW, u.r.flags);
  TEST_ASSERT_EQUAL(0, u.r.reinits);
}

static void test_a_silence_is_written_once_it_has_lasted_and_then_at_each_doubling() {
  Unit u;
  u.contact(0);
  int saves = 0;
  uint8_t at[16] = {0};
  for (uint32_t t = AFTER; t <= 24UL * 60 * MIN; t += 30000) {
    if (u.tick(t) & BUS_SILENCE_DO_SAVE) at[saves++] = u.r.lastMinutes;
  }
  const uint8_t want[] = {6, 12, 24, 48, 96, 192, 255};
  TEST_ASSERT_EQUAL(7, saves);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(want, at, 7);
  TEST_ASSERT_EQUAL(255, u.r.longestMinutes);
  TEST_ASSERT_EQUAL(1, u.r.silences);
  TEST_ASSERT_EQUAL(255, u.r.reinits);  // saturated, not wrapped
  TEST_ASSERT_EQUAL(255, busSilenceNowMinutes(u.s, 24UL * 60 * MIN));
}

static void test_a_master_that_comes_and_goes_costs_no_write() {
  // One frame every five minutes, for a day: every gap is a silence, none is
  // long enough to be written.
  Unit u;
  u.contact(0);
  uint32_t t = 0;
  int saves = 0;
  for (int cycle = 0; cycle < 288; cycle++) {
    for (uint32_t end = t + 4 * MIN; t < end; t += 5000) {
      if (u.tick(t) & BUS_SILENCE_DO_SAVE) saves++;
    }
    u.frames++;
    if (u.tick(t) & BUS_SILENCE_DO_SAVE) saves++;
    t += MIN;
  }
  TEST_ASSERT_EQUAL(0, saves);
  TEST_ASSERT_EQUAL(255, u.r.silences);  // counted all the same, until a restart
  TEST_ASSERT_EQUAL(5, u.r.lastMinutes);
}

static void test_contact_ends_a_silence_and_its_length_is_kept() {
  Unit u;
  u.contact(0);
  for (uint32_t t = AFTER; t < 7 * MIN; t += 1000) u.tick(t);
  u.frames++;
  TEST_ASSERT_EQUAL_HEX8(BUS_SILENCE_DO_SAVE, u.tick(7 * MIN + 30000));
  TEST_ASSERT_EQUAL(7, u.r.lastMinutes);
  TEST_ASSERT_EQUAL(7, u.r.longestMinutes);
  TEST_ASSERT_TRUE(u.r.flags & BUS_RECORD_FLAG_ENDED);
  TEST_ASSERT_EQUAL(0, busSilenceNowMinutes(u.s, 7 * MIN + 30000));
  // A shorter one later moves the latest, not the longest, and is not written.
  const uint32_t t0 = 20 * MIN;
  u.frames++;
  u.tick(t0);
  for (uint32_t t = t0 + AFTER; t < t0 + 4 * MIN; t += 1000) u.tick(t);
  TEST_ASSERT_EQUAL(2, u.r.silences);
  TEST_ASSERT_FALSE(u.r.flags & BUS_RECORD_FLAG_ENDED);  // the new one is open
  u.frames++;
  TEST_ASSERT_EQUAL_HEX8(0, u.tick(t0 + 4 * MIN));
  TEST_ASSERT_EQUAL(4, u.r.lastMinutes);
  TEST_ASSERT_EQUAL(7, u.r.longestMinutes);
  TEST_ASSERT_TRUE(u.r.flags & BUS_RECORD_FLAG_ENDED);
}

static void test_contact_right_after_a_bus_restart_is_counted_as_following_it() {
  Unit u;
  u.contact(0);
  u.tick(AFTER);  // restart of the bus hardware
  u.frames++;
  u.tick(AFTER + BUS_SILENCE_REINIT_HEARD_MS);
  TEST_ASSERT_EQUAL(1, u.r.reinitsHeard);
  // Contact that comes back half a span after one is not put down to it.
  const uint32_t t0 = AFTER + BUS_SILENCE_REINIT_HEARD_MS;
  u.tick(t0 + AFTER);
  TEST_ASSERT_EQUAL(2, u.r.silences);
  u.frames++;
  u.tick(t0 + AFTER + AFTER / 2);
  TEST_ASSERT_EQUAL(1, u.r.reinitsHeard);
}

static void test_traffic_for_others_is_noted_and_the_units_own_frame_is_not() {
  Unit u;
  u.contact(0);
  u.tick(AFTER);
  // Edges on the lines, then a frame for this unit: that was the frame.
  TEST_ASSERT_EQUAL_HEX8(0, u.tick(AFTER + 2000, false, true));
  u.frames++;
  u.tick(AFTER + 2100);
  TEST_ASSERT_FALSE(u.r.flags & BUS_RECORD_FLAG_TRAFFIC);
  // Next silence: edges and still nothing for this unit a second later.
  const uint32_t t0 = AFTER + 2100 + AFTER;
  u.tick(t0);
  TEST_ASSERT_EQUAL(2, u.s.silent ? u.r.silences : 0);
  TEST_ASSERT_EQUAL_HEX8(0, u.tick(t0 + 500, false, true));
  TEST_ASSERT_EQUAL_HEX8(0, u.tick(t0 + 1400));
  TEST_ASSERT_FALSE(u.r.flags & BUS_RECORD_FLAG_TRAFFIC);
  // Noted; written with the next mark of the length, not by itself.
  TEST_ASSERT_EQUAL_HEX8(0, u.tick(t0 + 1500));
  TEST_ASSERT_TRUE(u.r.flags & BUS_RECORD_FLAG_TRAFFIC);
  TEST_ASSERT_EQUAL_HEX8(BUS_SILENCE_DO_REINIT | BUS_SILENCE_DO_SAVE, u.tick(t0 + AFTER));
}

static void test_the_unit_restarts_itself_once_and_only_behind_its_gate() {
  Unit off;
  off.contact(0);
  for (uint32_t t = AFTER; t <= 120 * MIN; t += 1000) {
    TEST_ASSERT_FALSE(off.tick(t) & BUS_SILENCE_DO_RESTART);
  }
  TEST_ASSERT_EQUAL(0, off.r.selfRestarts);

  Unit on;
  on.gate = true;
  on.contact(0);
  int restarts = 0;
  for (uint32_t t = AFTER; t <= 120 * MIN; t += 1000) {
    const uint8_t todo = on.tick(t);
    if (todo & BUS_SILENCE_DO_RESTART) {
      restarts++;
      TEST_ASSERT_EQUAL_UINT32(RESTART, t);
      // The record is written before the unit goes down.
      TEST_ASSERT_TRUE(todo & BUS_SILENCE_DO_SAVE);
      TEST_ASSERT_EQUAL(20, on.r.lastMinutes);
    }
  }
  TEST_ASSERT_EQUAL(1, restarts);
  TEST_ASSERT_EQUAL(1, on.r.selfRestarts);
  TEST_ASSERT_TRUE(on.r.flags & BUS_RECORD_FLAG_RESTARTED);
}

static void test_a_unit_that_restarted_waits_for_contact_before_it_counts_again() {
  // After its restart the unit starts from nothing with the stored record.
  Unit u;
  u.gate = true;
  u.r.silences = 1;
  u.r.selfRestarts = 1;
  u.r.flags = BUS_RECORD_FLAG_RESTARTED;
  for (uint32_t t = 0; t < 240 * MIN; t += 1000) TEST_ASSERT_EQUAL_HEX8(0, u.tick(t));
  TEST_ASSERT_EQUAL(1, u.r.silences);
  TEST_ASSERT_EQUAL(1, u.r.selfRestarts);
}

static void test_the_count_of_frames_may_wrap() {
  Unit u;
  u.frames = 0xFFFC;
  u.s.lastFrames = 0xFFFC;
  u.contact(0);  // wraps to 0x0004
  TEST_ASSERT_EQUAL_HEX8(0, u.tick(AFTER - 1));
  TEST_ASSERT_TRUE(u.tick(AFTER) & BUS_SILENCE_DO_REINIT);
}

static void test_the_clock_may_wrap_inside_a_silence() {
  Unit u;
  const uint32_t t0 = 0xFFFFFFFFUL - 30000UL;
  u.s.heardMs = t0;
  u.contact(t0);
  TEST_ASSERT_EQUAL_HEX8(0, u.tick(t0 + 20000));
  TEST_ASSERT_TRUE(u.tick(t0 + AFTER) & BUS_SILENCE_DO_REINIT);  // past the wrap
  TEST_ASSERT_EQUAL(1, u.r.silences);
  TEST_ASSERT_TRUE(u.tick(t0 + 2 * AFTER) & BUS_SILENCE_DO_SAVE);
}

// --- the record on the wire and in EEPROM ---------------------------------------------

static UnitBusRecord sample() {
  UnitBusRecord r;
  r.silences = 3;
  r.lastMinutes = 120;
  r.longestMinutes = 200;
  r.reinits = 150;
  r.reinitsHeard = 1;
  r.selfRestarts = 2;
  r.flags = BUS_RECORD_FLAG_TRAFFIC | BUS_RECORD_FLAG_RESTARTED;
  return r;
}

static void test_the_reply_reads_back_what_was_sent() {
  uint8_t buf[BUS_RECORD_REPLY_LEN];
  busRecordEncodeReply(sample(), 17, buf);
  UnitBusRecord out;
  uint8_t now = 0;
  TEST_ASSERT_TRUE(busRecordReadbackValid(buf, sizeof(buf), out, now));
  TEST_ASSERT_EQUAL(3, out.silences);
  TEST_ASSERT_EQUAL(120, out.lastMinutes);
  TEST_ASSERT_EQUAL(200, out.longestMinutes);
  TEST_ASSERT_EQUAL(150, out.reinits);
  TEST_ASSERT_EQUAL(1, out.reinitsHeard);
  TEST_ASSERT_EQUAL(2, out.selfRestarts);
  TEST_ASSERT_EQUAL_HEX8(BUS_RECORD_FLAG_TRAFFIC | BUS_RECORD_FLAG_RESTARTED, out.flags);
  TEST_ASSERT_EQUAL(17, now);
}

static void test_what_an_older_unit_answers_is_no_record() {
  UnitBusRecord out = sample();
  uint8_t now = 9;
  uint8_t buf[BUS_RECORD_REPLY_LEN];
  // Its one status byte and bus padding; nothing at all; a line stuck low.
  for (uint8_t first : {0x00, 0x01, 0xFF}) {
    for (uint8_t pad : {0xFF, 0x00}) {
      memset(buf, pad, sizeof(buf));
      buf[0] = first;
      TEST_ASSERT_FALSE(busRecordReadbackValid(buf, sizeof(buf), out, now));
    }
  }
  // A short read, and one changed bit.
  busRecordEncodeReply(sample(), 0, buf);
  TEST_ASSERT_FALSE(busRecordReadbackValid(buf, BUS_RECORD_REPLY_LEN - 1, out, now));
  buf[2] ^= 0x10;
  TEST_ASSERT_FALSE(busRecordReadbackValid(buf, sizeof(buf), out, now));
  // A rejected read leaves what the reader had.
  TEST_ASSERT_EQUAL(3, out.silences);
  TEST_ASSERT_EQUAL(9, now);
}

static void test_the_eeprom_block_reads_back_and_a_blank_one_is_an_empty_record() {
  uint8_t block[EE_BUS_RECORD_BLOCK_LEN];
  unitEeBusRecordEncode(sample(), block);
  UnitBusRecord out;
  TEST_ASSERT_TRUE(unitEeBusRecordDecode(block, out));
  TEST_ASSERT_EQUAL(200, out.longestMinutes);
  TEST_ASSERT_EQUAL(2, out.selfRestarts);
  for (uint8_t fill : {0xFF, 0x00}) {
    memset(block, fill, sizeof(block));
    out = sample();
    TEST_ASSERT_FALSE(unitEeBusRecordDecode(block, out));
    TEST_ASSERT_EQUAL(0, out.silences);
    TEST_ASSERT_EQUAL(0, out.reinits);
  }
  unitEeBusRecordEncode(sample(), block);
  block[1] ^= 0x01;
  TEST_ASSERT_FALSE(unitEeBusRecordDecode(block, out));
  TEST_ASSERT_EQUAL(0, out.silences);
}

static void test_the_restart_gate_is_a_gate_this_firmware_knows() {
  TEST_ASSERT_TRUE(unitGateBitsKnown(UNIT_GATE_SILENCE_RESTART));
  TEST_ASSERT_EQUAL_HEX8(UNIT_GATE_SILENCE_RESTART, SFP_UNIT_GATE_SILENCE_RESTART);
  TEST_ASSERT_EQUAL_HEX8(0, SFP_UNIT_GATE_IMPLEMENTED & (uint8_t)~UNIT_GATE_ALL);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_the_spans_leave_room_for_what_a_master_does);
  RUN_TEST(test_a_unit_nobody_addressed_has_no_silence);
  RUN_TEST(test_a_span_without_a_frame_is_a_silence_and_restarts_the_bus_hardware);
  RUN_TEST(test_the_bus_hardware_is_restarted_every_span_and_never_under_a_held_line);
  RUN_TEST(test_a_line_held_low_at_the_start_is_noted);
  RUN_TEST(test_a_silence_is_written_once_it_has_lasted_and_then_at_each_doubling);
  RUN_TEST(test_a_master_that_comes_and_goes_costs_no_write);
  RUN_TEST(test_contact_ends_a_silence_and_its_length_is_kept);
  RUN_TEST(test_contact_right_after_a_bus_restart_is_counted_as_following_it);
  RUN_TEST(test_traffic_for_others_is_noted_and_the_units_own_frame_is_not);
  RUN_TEST(test_the_unit_restarts_itself_once_and_only_behind_its_gate);
  RUN_TEST(test_a_unit_that_restarted_waits_for_contact_before_it_counts_again);
  RUN_TEST(test_the_count_of_frames_may_wrap);
  RUN_TEST(test_the_clock_may_wrap_inside_a_silence);
  RUN_TEST(test_the_reply_reads_back_what_was_sent);
  RUN_TEST(test_what_an_older_unit_answers_is_no_record);
  RUN_TEST(test_the_eeprom_block_reads_back_and_a_blank_one_is_an_empty_record);
  RUN_TEST(test_the_restart_gate_is_a_gate_this_firmware_knows);
  return UNITY_END();
}
