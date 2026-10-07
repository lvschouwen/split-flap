// Native tests for WallWatchPolicy.h: which changes of a verdict are recorded.
#include <unity.h>

#include <vector>

#include "../../WallWatchPolicy.h"

void setUp() {}
void tearDown() {}

struct Entry {
  EventKind kind;
  uint8_t detail;
  uint8_t unit;
  uint32_t a;
  uint32_t b;
};

struct Sink {
  std::vector<Entry> entries;
  void event(EventKind kind, uint8_t detail, uint8_t unit, uint32_t a, uint32_t b) {
    entries.push_back({kind, detail, unit, a, b});
  }
  int count(EventKind kind, uint8_t detail) const {
    int n = 0;
    for (const Entry& e : entries) n += (e.kind == kind && e.detail == detail) ? 1 : 0;
    return n;
  }
};

static UnitFacts healthy(uint16_t uptime = 600) {
  UnitFacts u;
  u.state = 1;
  u.fwStatus = 0;
  u.statusValid = true;
  u.status.flags = UNIT_FLAG_HOMED;
  u.status.uptimeSeconds = uptime;
  u.bootVerdict = BOOT_INTEGRITY_OK;
  u.vitalsValid = true;
  u.vitals.vccMin_mV = 4900;
  u.diagValid = true;
  u.extDiagValid = true;
  u.extDiag.hallEdgesLastRev = 1;
  u.lifetimeValid = true;
  return u;
}

struct Row {
  UnitFacts units[UNITS_AMOUNT];
  UnitVerdict verdicts[UNITS_AMOUNT];
  WatchBoard watch;
  Sink sink;
  int width = 3;
  Row() {
    for (int i = 0; i < width; i++) units[i] = healthy();
  }
  void look(bool hold = false, uint32_t nowMs = 100000) {
    sink.entries.clear();
    watchUnits(watch, units, width, nowMs, hold, verdicts, sink);
  }
};

static const uint8_t U = (uint8_t)UnitReason::NotAnswering;

static void test_a_healthy_row_records_nothing() {
  Row row;
  row.look();
  TEST_ASSERT_EQUAL(0, (int)row.sink.entries.size());
  row.look();
  TEST_ASSERT_EQUAL(0, (int)row.sink.entries.size());
  TEST_ASSERT_TRUE(VerdictLevel::Working == row.verdicts[2].level);
}

static void test_a_unit_going_quiet_and_coming_back_is_two_entries() {
  Row row;
  row.look();
  row.units[1].stale = true;
  row.units[1].lastSeenMs = 40000;
  row.units[1].misses = 5;
  row.look();
  TEST_ASSERT_EQUAL(1, (int)row.sink.entries.size());
  const Entry& on = row.sink.entries[0];
  TEST_ASSERT_TRUE(EventKind::UnitReasonOn == on.kind);
  TEST_ASSERT_EQUAL_UINT8(U, on.detail);
  TEST_ASSERT_EQUAL_UINT8(SFP_I2C_ADDRESS_BASE + 1, on.unit);
  TEST_ASSERT_EQUAL_UINT32(60, on.a);
  TEST_ASSERT_EQUAL_UINT32(5, on.b);
  row.look();  // still quiet: said once
  TEST_ASSERT_EQUAL(0, (int)row.sink.entries.size());
  row.units[1].stale = false;
  row.look();
  TEST_ASSERT_EQUAL(1, (int)row.sink.entries.size());
  TEST_ASSERT_TRUE(EventKind::UnitReasonOff == row.sink.entries[0].kind);
  TEST_ASSERT_EQUAL_UINT8(U, row.sink.entries[0].detail);
}

static void test_a_quiet_unit_does_not_recover_from_what_it_last_said() {
  Row row;
  row.look();
  row.units[0].status.flags |= UNIT_FLAG_LAST_HOME_FAILED;
  row.look();
  TEST_ASSERT_EQUAL(1, row.sink.count(EventKind::UnitReasonOn, (uint8_t)UnitReason::HomeFailed));
  // It goes quiet: its failed home is not over, it is unknown.
  row.units[0].stale = true;
  row.look();
  TEST_ASSERT_EQUAL(1, row.sink.count(EventKind::UnitReasonOn, U));
  TEST_ASSERT_EQUAL(0, row.sink.count(EventKind::UnitReasonOff, (uint8_t)UnitReason::HomeFailed));
  // Back, and homed: now both end.
  row.units[0].stale = false;
  row.units[0].status.flags = UNIT_FLAG_HOMED;
  row.look();
  TEST_ASSERT_EQUAL(1, row.sink.count(EventKind::UnitReasonOff, U));
  TEST_ASSERT_EQUAL(1, row.sink.count(EventKind::UnitReasonOff, (uint8_t)UnitReason::HomeFailed));
}

static void test_a_status_that_did_not_arrive_changes_nothing() {
  Row row;
  row.units[2].status.flags |= UNIT_FLAG_HALL_NEVER;
  row.look();
  row.units[2].statusValid = false;
  row.look();
  TEST_ASSERT_EQUAL(0, (int)row.sink.entries.size());
  row.units[2].statusValid = true;
  row.look();
  TEST_ASSERT_EQUAL(0, (int)row.sink.entries.size());
}

static void test_the_first_look_records_faults_and_not_notes() {
  Row row;
  row.units[0].vitals.vccMin_mV = 3800;                  // a note
  row.units[1].status.flags |= UNIT_FLAG_LAST_HOME_FAILED;  // a fault
  row.look();
  TEST_ASSERT_EQUAL(1, (int)row.sink.entries.size());
  TEST_ASSERT_EQUAL_UINT8((uint8_t)UnitReason::HomeFailed, row.sink.entries[0].detail);
  // The note it started with is never announced later either.
  row.look();
  TEST_ASSERT_EQUAL(0, (int)row.sink.entries.size());
}

static void test_a_note_that_starts_later_is_recorded_once_and_its_end_is_not() {
  Row row;
  row.look();
  row.units[0].vitals.vccMin_mV = 3800;
  row.look();
  TEST_ASSERT_EQUAL(1, (int)row.sink.entries.size());
  const Entry& e = row.sink.entries[0];
  TEST_ASSERT_EQUAL_UINT8((uint8_t)UnitReason::LowSupply, e.detail);
  TEST_ASSERT_EQUAL_UINT32(3800, e.a);
  TEST_ASSERT_EQUAL_UINT32(UNIT_VCC_MIN_FLOOR_MV, e.b);
  row.units[0].vitals.vccMin_mV = 4900;
  row.look();
  TEST_ASSERT_EQUAL(0, (int)row.sink.entries.size());
}

static void test_reasons_that_come_and_go_by_design_are_not_recorded() {
  Row row;
  row.look();
  row.units[0].status.flags = UNIT_FLAG_MOVING;  // finding home
  row.units[1].fwStatus = 1;                     // firmware behind
  row.units[2].bootVerdict = BOOT_INTEGRITY_OUTDATED;
  row.units[2].lifetime.homeFailedCount = 2;
  row.look();
  TEST_ASSERT_EQUAL(0, (int)row.sink.entries.size());
  TEST_ASSERT_TRUE(VerdictLevel::Note == row.verdicts[0].level);
}

static void test_a_unit_restart_is_recorded_with_its_cause() {
  Row row;
  row.look();
  row.units[1].status.uptimeSeconds = 12;
  row.units[1].status.mcusrAtBoot = 0x04;
  row.units[1].status.lifetimeBrownoutCount = 3;
  row.units[1].status.lifetimeWatchdogCount = 1;
  row.look();
  TEST_ASSERT_EQUAL(1, (int)row.sink.entries.size());
  const Entry& e = row.sink.entries[0];
  TEST_ASSERT_TRUE(EventKind::UnitRestarted == e.kind);
  TEST_ASSERT_EQUAL_UINT8(0x04, e.detail);
  TEST_ASSERT_EQUAL_UINT8(SFP_I2C_ADDRESS_BASE + 1, e.unit);
  TEST_ASSERT_EQUAL_UINT32(3, e.a);
  TEST_ASSERT_EQUAL_UINT32(1, e.b);
}

static void test_the_first_look_is_no_restart() {
  Row row;
  row.units[0].status.uptimeSeconds = 3;
  row.look();
  TEST_ASSERT_EQUAL(0, (int)row.sink.entries.size());
}

static void test_a_unit_update_is_watched_in_silence() {
  Row row;
  row.units[0].status.flags |= UNIT_FLAG_LAST_HOME_FAILED;
  row.look();
  // Updating: units sit in their bootloaders and restart.
  row.units[0] = UnitFacts();
  row.units[0].state = 2;
  row.units[1].status.uptimeSeconds = 2;
  row.look(true);
  TEST_ASSERT_EQUAL(0, (int)row.sink.entries.size());
  TEST_ASSERT_TRUE(UnitReason::BeingUpdated == row.verdicts[0].reason);
  // Done: the restarts of the update are not news, the failed home still stands.
  row.units[0] = healthy(5);
  row.units[0].status.flags |= UNIT_FLAG_LAST_HOME_FAILED;
  row.units[1].status.uptimeSeconds = 9;
  row.look();
  TEST_ASSERT_EQUAL(0, row.sink.count(EventKind::UnitRestarted, 0));
  TEST_ASSERT_EQUAL(0, row.sink.count(EventKind::UnitReasonOn, (uint8_t)UnitReason::HomeFailed));
  TEST_ASSERT_EQUAL(0, row.sink.count(EventKind::UnitReasonOff, (uint8_t)UnitReason::HomeFailed));
}

static void test_a_unit_left_in_its_bootloader_after_the_update_is_recorded() {
  Row row;
  row.look();
  row.units[2] = UnitFacts();
  row.units[2].state = 2;
  row.look(true);
  row.look();
  TEST_ASSERT_EQUAL(1, row.sink.count(EventKind::UnitReasonOn, (uint8_t)UnitReason::InBootloader));
}

static void test_a_worn_drum_is_judged_against_its_row() {
  Row row;
  for (int i = 0; i < row.width; i++) {
    row.units[i].odometerValid = true;
    row.units[i].odometer = 100;
  }
  row.look();
  row.units[1].odometer = 100 + WEAR_FLAG_FLOOR_REVS + 1;
  row.look();
  TEST_ASSERT_EQUAL(1, row.sink.count(EventKind::UnitReasonOn, (uint8_t)UnitReason::Worn));
  TEST_ASSERT_EQUAL_UINT32(100 + WEAR_FLAG_FLOOR_REVS + 1, row.sink.entries[0].a);
}

// ---- boards ------------------------------------------------------------------

struct Board {
  BoardFacts facts;
  WatchBoard watch;
  Sink sink;
  Board() {
    facts.unitsPlaced = 5;
    facts.unitsKnown = true;
    facts.unitsFound = 5;
  }
  void look() {
    sink.entries.clear();
    watchBoard(watch, facts, boardVerdict(facts), sink);
  }
};

static void test_a_row_lost_and_back_is_two_entries() {
  Board b;
  b.look();
  TEST_ASSERT_EQUAL(0, (int)b.sink.entries.size());
  b.facts.reach = BoardReach::Away;
  b.look();
  TEST_ASSERT_EQUAL(0, (int)b.sink.entries.size());  // briefly away is not news
  b.facts.reach = BoardReach::Lost;
  b.facts.silentS = 31;
  b.look();
  TEST_ASSERT_EQUAL(1, (int)b.sink.entries.size());
  TEST_ASSERT_TRUE(EventKind::BoardReasonOn == b.sink.entries[0].kind);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)BoardReason::Lost, b.sink.entries[0].detail);
  TEST_ASSERT_EQUAL_UINT8(0, b.sink.entries[0].unit);
  TEST_ASSERT_EQUAL_UINT32(31, b.sink.entries[0].a);
  b.look();
  TEST_ASSERT_EQUAL(0, (int)b.sink.entries.size());
  b.facts.reach = BoardReach::Up;
  b.look();
  TEST_ASSERT_EQUAL(1, b.sink.count(EventKind::BoardReasonOff, (uint8_t)BoardReason::Lost));
}

static void test_a_lost_row_keeps_its_dead_bus() {
  Board b;
  b.look();
  b.facts.busDead = true;
  b.facts.busEpisodes = 1;
  b.look();
  TEST_ASSERT_EQUAL(1, b.sink.count(EventKind::BoardReasonOn, (uint8_t)BoardReason::BusDead));
  b.facts.reach = BoardReach::Lost;
  b.look();
  TEST_ASSERT_EQUAL(0, b.sink.count(EventKind::BoardReasonOff, (uint8_t)BoardReason::BusDead));
  b.facts.reach = BoardReach::Up;
  b.facts.busDead = false;
  b.look();
  TEST_ASSERT_EQUAL(1, b.sink.count(EventKind::BoardReasonOff, (uint8_t)BoardReason::BusDead));
  TEST_ASSERT_EQUAL(1, b.sink.count(EventKind::BoardReasonOff, (uint8_t)BoardReason::Lost));
}

static void test_an_update_is_recorded_from_start_to_end() {
  Board b;
  b.look();
  b.facts.updating = true;
  b.look();
  TEST_ASSERT_EQUAL(1, b.sink.count(EventKind::BoardReasonOn, (uint8_t)BoardReason::Updating));
  b.facts.reach = BoardReach::Lost;  // it restarts into the image
  b.look();
  TEST_ASSERT_EQUAL(0, (int)b.sink.entries.size());
  b.facts.updating = false;
  b.facts.reach = BoardReach::Up;
  b.look();
  TEST_ASSERT_EQUAL(1, (int)b.sink.entries.size());
  TEST_ASSERT_EQUAL(1, b.sink.count(EventKind::BoardReasonOff, (uint8_t)BoardReason::Updating));
}

static void test_the_units_own_troubles_are_not_the_boards_entries() {
  Board b;
  b.look();
  b.facts.unitsFault = 2;
  b.facts.unitsNote = 1;
  b.facts.clockSet = false;
  b.facts.firmwareDiffers = true;
  b.look();
  TEST_ASSERT_EQUAL(0, (int)b.sink.entries.size());
  b.facts.unitsFound = 4;
  b.look();
  TEST_ASSERT_EQUAL(1, b.sink.count(EventKind::BoardReasonOn, (uint8_t)BoardReason::UnitsMissing));
  TEST_ASSERT_EQUAL_UINT32(4, b.sink.entries[0].a);
  TEST_ASSERT_EQUAL_UINT32(5, b.sink.entries[0].b);
}

static void test_the_first_look_at_a_board_records_what_is_wrong_or_going_on() {
  Board b;
  b.facts.updatingUnits = true;
  b.look();
  TEST_ASSERT_EQUAL(1, b.sink.count(EventKind::BoardReasonOn, (uint8_t)BoardReason::UpdatingUnits));
  Board c;
  c.facts.rescue = true;
  c.look();
  TEST_ASSERT_EQUAL(1, c.sink.count(EventKind::BoardReasonOn, (uint8_t)BoardReason::Rescue));
  Board d;
  d.facts.unitsNote = 2;
  d.facts.firmwareDiffers = true;
  d.look();
  TEST_ASSERT_EQUAL(0, (int)d.sink.entries.size());
}

// What ends was recorded as started: an end never stands alone.
static void test_a_board_paired_while_it_takes_its_image_has_both_ends_recorded() {
  Board b;
  b.facts.updating = true;
  b.look();
  TEST_ASSERT_EQUAL(1, b.sink.count(EventKind::BoardReasonOn, (uint8_t)BoardReason::Updating));
  b.facts.updating = false;
  b.look();
  TEST_ASSERT_EQUAL(1, b.sink.count(EventKind::BoardReasonOff, (uint8_t)BoardReason::Updating));
}

static void test_a_row_restart_is_recorded_with_its_rev() {
  WatchBoard w;
  Sink sink;
  watchRowRestarts(w, 0, false, "97e2679", sink);
  watchRowRestarts(w, 0, false, "97e2679", sink);
  TEST_ASSERT_EQUAL(0, (int)sink.entries.size());
  watchRowRestarts(w, 1, true, "68007c2", sink);
  TEST_ASSERT_EQUAL(1, (int)sink.entries.size());
  TEST_ASSERT_TRUE(EventKind::RowStarted == sink.entries[0].kind);
  TEST_ASSERT_EQUAL_UINT8(1, sink.entries[0].detail);
  TEST_ASSERT_EQUAL_HEX32(0x68007C2, sink.entries[0].a);
  watchRowRestarts(w, 1, true, "68007c2", sink);
  TEST_ASSERT_EQUAL(1, (int)sink.entries.size());
  // The link's count starts over when the rows table changes.
  watchRowRestarts(w, 0, false, "68007c2", sink);
  TEST_ASSERT_EQUAL(1, (int)sink.entries.size());
  watchRowRestarts(w, 1, false, "68007c2", sink);
  TEST_ASSERT_EQUAL(2, (int)sink.entries.size());
}

static void test_every_recorded_reason_is_a_reason() {
  TEST_ASSERT_EQUAL_UINT32(0, unitRecordedReasons() >> UNIT_REASON_COUNT);
  TEST_ASSERT_EQUAL_UINT32(0, unitRecordedReasons() & unitReasonBit(UnitReason::Working));
  // A fault's end is recorded, so its start must be.
  TEST_ASSERT_EQUAL_UINT32(unitFaultReasons(), unitFaultReasons() & unitRecordedReasons());
  TEST_ASSERT_EQUAL_UINT32(0, boardRecordedReasons() >> BOARD_REASON_COUNT);
  const uint32_t unitsOwn = boardReasonBit(BoardReason::UnitsFault);
  TEST_ASSERT_EQUAL_UINT32(boardFaultReasons() & ~unitsOwn,
                           boardFaultReasons() & boardRecordedReasons());
}

static void test_the_table_follows_boards_by_key_and_forgets_those_that_left() {
  WatchTable<3> table;
  WatchBoard* a = table.find(0);
  WatchBoard* b = table.find(0x1234);
  TEST_ASSERT_NOT_NULL(a);
  TEST_ASSERT_NOT_NULL(b);
  TEST_ASSERT_TRUE(a != b);
  b->boardReasons = 77;
  TEST_ASSERT_TRUE(table.find(0x1234) == b);
  TEST_ASSERT_EQUAL_UINT32(77, table.find(0x1234)->boardReasons);
  TEST_ASSERT_NOT_NULL(table.find(0x2222));
  TEST_ASSERT_NULL(table.find(0x3333));
  const uint16_t keys[] = {0, 0x2222};
  table.keep(keys, 2);
  WatchBoard* fresh = table.find(0x3333);
  TEST_ASSERT_NOT_NULL(fresh);
  TEST_ASSERT_EQUAL_UINT32(0, fresh->boardReasons);
  TEST_ASSERT_FALSE(fresh->unitsSeen);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_a_healthy_row_records_nothing);
  RUN_TEST(test_a_unit_going_quiet_and_coming_back_is_two_entries);
  RUN_TEST(test_a_quiet_unit_does_not_recover_from_what_it_last_said);
  RUN_TEST(test_a_status_that_did_not_arrive_changes_nothing);
  RUN_TEST(test_the_first_look_records_faults_and_not_notes);
  RUN_TEST(test_a_note_that_starts_later_is_recorded_once_and_its_end_is_not);
  RUN_TEST(test_reasons_that_come_and_go_by_design_are_not_recorded);
  RUN_TEST(test_a_unit_restart_is_recorded_with_its_cause);
  RUN_TEST(test_the_first_look_is_no_restart);
  RUN_TEST(test_a_unit_update_is_watched_in_silence);
  RUN_TEST(test_a_unit_left_in_its_bootloader_after_the_update_is_recorded);
  RUN_TEST(test_a_worn_drum_is_judged_against_its_row);
  RUN_TEST(test_a_row_lost_and_back_is_two_entries);
  RUN_TEST(test_a_lost_row_keeps_its_dead_bus);
  RUN_TEST(test_an_update_is_recorded_from_start_to_end);
  RUN_TEST(test_the_units_own_troubles_are_not_the_boards_entries);
  RUN_TEST(test_the_first_look_at_a_board_records_what_is_wrong_or_going_on);
  RUN_TEST(test_a_board_paired_while_it_takes_its_image_has_both_ends_recorded);
  RUN_TEST(test_a_row_restart_is_recorded_with_its_rev);
  RUN_TEST(test_every_recorded_reason_is_a_reason);
  RUN_TEST(test_the_table_follows_boards_by_key_and_forgets_those_that_left);
  return UNITY_END();
}
