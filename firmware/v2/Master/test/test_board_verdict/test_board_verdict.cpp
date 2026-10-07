// Native tests for BoardVerdict.h: what the master makes of one board.
#include <unity.h>

#include "../../BoardVerdict.h"

void setUp() {}
void tearDown() {}

// A row board with nothing to remark.
static BoardFacts healthyRow() {
  BoardFacts f;
  f.unitsPlaced = 5;
  f.unitsKnown = true;
  f.unitsFound = 5;
  f.uptimeS = 3600;
  return f;
}

static bool has(const BoardVerdict& v, BoardReason r) { return (v.all & boardReasonBit(r)) != 0; }

static void test_a_healthy_board_is_working() {
  const BoardVerdict v = boardVerdict(healthyRow());
  TEST_ASSERT_TRUE(VerdictLevel::Working == v.level);
  TEST_ASSERT_TRUE(BoardReason::Working == v.reason);
  TEST_ASSERT_EQUAL_UINT32(5, v.a);
  TEST_ASSERT_EQUAL_UINT32(3600, v.b);
  TEST_ASSERT_EQUAL_UINT32(0, v.all);
}

static void test_a_lost_row_is_a_fault_and_judged_on_nothing_else() {
  BoardFacts f = healthyRow();
  f.reach = BoardReach::Lost;
  f.silentS = 95;
  f.unitsFault = 2;
  f.busDead = true;
  const BoardVerdict v = boardVerdict(f);
  TEST_ASSERT_TRUE(VerdictLevel::Fault == v.level);
  TEST_ASSERT_TRUE(BoardReason::Lost == v.reason);
  TEST_ASSERT_EQUAL_UINT32(95, v.a);
  TEST_ASSERT_EQUAL_UINT32(boardReasonBit(BoardReason::Lost), v.all);
}

static void test_a_row_briefly_away_or_busy_is_no_fault() {
  BoardFacts f = healthyRow();
  f.reach = BoardReach::Away;
  f.silentS = 12;
  BoardVerdict v = boardVerdict(f);
  TEST_ASSERT_TRUE(VerdictLevel::Note == v.level);
  TEST_ASSERT_TRUE(BoardReason::Away == v.reason);
  TEST_ASSERT_EQUAL_UINT32(12, v.a);
  f.reach = BoardReach::Busy;
  TEST_ASSERT_TRUE(VerdictLevel::Working == boardVerdict(f).level);
}

static void test_a_row_never_heard_is_waited_for_and_then_a_fault() {
  BoardFacts f = healthyRow();
  f.reach = BoardReach::Never;
  f.unitsKnown = false;
  f.startGraceOver = false;
  TEST_ASSERT_TRUE(BoardReason::Away == boardVerdict(f).reason);
  f.startGraceOver = true;
  const BoardVerdict v = boardVerdict(f);
  TEST_ASSERT_TRUE(VerdictLevel::Fault == v.level);
  TEST_ASSERT_TRUE(BoardReason::NeverSeen == v.reason);
}

static void test_a_row_taking_its_image_is_updating_even_while_gone() {
  BoardFacts f = healthyRow();
  f.updating = true;
  f.reach = BoardReach::Lost;
  const BoardVerdict v = boardVerdict(f);
  TEST_ASSERT_TRUE(VerdictLevel::Note == v.level);
  TEST_ASSERT_TRUE(BoardReason::Updating == v.reason);
}

static void test_rescue_mode_is_a_fault() {
  BoardFacts f = healthyRow();
  f.rescue = true;
  f.unitsKnown = false;
  const BoardVerdict v = boardVerdict(f);
  TEST_ASSERT_TRUE(VerdictLevel::Fault == v.level);
  TEST_ASSERT_TRUE(BoardReason::Rescue == v.reason);
  TEST_ASSERT_FALSE(has(v, BoardReason::UnitsUnknown));
}

static void test_a_dead_bus_is_one_fault_not_one_per_unit() {
  BoardFacts f = healthyRow();
  f.busDead = true;
  f.busEpisodes = 3;
  f.unitsFault = 5;
  const BoardVerdict v = boardVerdict(f);
  TEST_ASSERT_TRUE(BoardReason::BusDead == v.reason);
  TEST_ASSERT_EQUAL_UINT32(3, v.a);
  TEST_ASSERT_FALSE(has(v, BoardReason::UnitsFault));
}

static void test_an_image_given_up_on_is_a_fault() {
  BoardFacts f = healthyRow();
  f.updateBlocked = true;
  f.updateAttempts = 3;
  f.firmwareDiffers = true;
  const BoardVerdict v = boardVerdict(f);
  TEST_ASSERT_TRUE(VerdictLevel::Fault == v.level);
  TEST_ASSERT_TRUE(BoardReason::UpdateBlocked == v.reason);
  TEST_ASSERT_EQUAL_UINT32(3, v.a);
  TEST_ASSERT_TRUE(has(v, BoardReason::FirmwareDiffers));
}

static void test_units_missing_and_units_at_fault() {
  BoardFacts f = healthyRow();
  f.unitsFound = 4;
  BoardVerdict v = boardVerdict(f);
  TEST_ASSERT_TRUE(VerdictLevel::Fault == v.level);
  TEST_ASSERT_TRUE(BoardReason::UnitsMissing == v.reason);
  TEST_ASSERT_EQUAL_UINT32(4, v.a);
  TEST_ASSERT_EQUAL_UINT32(5, v.b);
  f = healthyRow();
  f.unitsFault = 1;
  f.unitsNote = 2;
  v = boardVerdict(f);
  TEST_ASSERT_TRUE(BoardReason::UnitsFault == v.reason);
  TEST_ASSERT_EQUAL_UINT32(1, v.a);
  TEST_ASSERT_EQUAL_UINT32(5, v.b);
  TEST_ASSERT_TRUE(has(v, BoardReason::UnitsNote));
}

static void test_more_units_than_placed_is_not_missing() {
  BoardFacts f = healthyRow();
  f.unitsFound = 6;
  TEST_ASSERT_TRUE(BoardReason::Working == boardVerdict(f).reason);
}

static void test_the_notes() {
  BoardFacts f = healthyRow();
  f.unitsNote = 3;
  BoardVerdict v = boardVerdict(f);
  TEST_ASSERT_TRUE(VerdictLevel::Note == v.level);
  TEST_ASSERT_TRUE(BoardReason::UnitsNote == v.reason);
  TEST_ASSERT_EQUAL_UINT32(3, v.a);
  f.firmwareDiffers = true;
  TEST_ASSERT_TRUE(BoardReason::FirmwareDiffers == boardVerdict(f).reason);
  f.clockSet = false;
  TEST_ASSERT_TRUE(BoardReason::ClockNotSet == boardVerdict(f).reason);
  f.unitsKnown = false;
  v = boardVerdict(f);
  TEST_ASSERT_TRUE(BoardReason::UnitsUnknown == v.reason);
  TEST_ASSERT_FALSE(has(v, BoardReason::UnitsNote));
}

static void test_a_board_updating_its_units_is_not_judged_on_them() {
  BoardFacts f = healthyRow();
  f.updatingUnits = true;
  f.unitsFault = 4;
  f.unitsFound = 1;
  const BoardVerdict v = boardVerdict(f);
  TEST_ASSERT_TRUE(VerdictLevel::Note == v.level);
  TEST_ASSERT_TRUE(BoardReason::UpdatingUnits == v.reason);
  TEST_ASSERT_EQUAL_UINT32(boardReasonBit(BoardReason::UpdatingUnits), v.all);
}

static void test_the_masters_own_row_is_judged_on_its_units_only() {
  BoardFacts f = healthyRow();
  f.own = true;
  f.reach = BoardReach::Never;  // link facts mean nothing for it
  f.rescue = true;
  f.updateBlocked = true;
  f.firmwareDiffers = true;
  TEST_ASSERT_TRUE(BoardReason::Working == boardVerdict(f).reason);
  f.unitsFault = 1;
  TEST_ASSERT_TRUE(BoardReason::UnitsFault == boardVerdict(f).reason);
}

static void test_the_reason_numbers_and_names_are_fixed() {
  static const struct { BoardReason r; uint8_t n; const char* name; } fixed[] = {
      {BoardReason::Working, 0, "working"},
      {BoardReason::Lost, 1, "lost"},
      {BoardReason::NeverSeen, 2, "never-seen"},
      {BoardReason::Rescue, 3, "rescue"},
      {BoardReason::BusDead, 4, "bus-dead"},
      {BoardReason::UpdateBlocked, 5, "update-blocked"},
      {BoardReason::UnitsMissing, 6, "units-missing"},
      {BoardReason::UnitsFault, 7, "units-fault"},
      {BoardReason::Updating, 8, "updating"},
      {BoardReason::UpdatingUnits, 9, "updating-units"},
      {BoardReason::Away, 10, "away"},
      {BoardReason::UnitsUnknown, 11, "units-unknown"},
      {BoardReason::ClockNotSet, 12, "clock-not-set"},
      {BoardReason::FirmwareDiffers, 13, "firmware-differs"},
      {BoardReason::UnitsNote, 14, "units-note"},
  };
  TEST_ASSERT_EQUAL(BOARD_REASON_COUNT, (int)(sizeof(fixed) / sizeof(fixed[0])));
  for (const auto& f : fixed) {
    TEST_ASSERT_EQUAL_UINT8(f.n, (uint8_t)f.r);
    TEST_ASSERT_EQUAL_STRING(f.name, boardReasonName(f.r));
  }
}

static void test_every_reason_is_ranked_once_and_faults_come_first() {
  uint32_t seen = 0;
  bool notes = false;
  for (BoardReason r : BOARD_REASON_ORDER) {
    TEST_ASSERT_TRUE(r != BoardReason::Working);
    TEST_ASSERT_EQUAL_UINT32(0, seen & boardReasonBit(r));
    seen |= boardReasonBit(r);
    if (boardReasonLevel(r) == VerdictLevel::Note) notes = true;
    else TEST_ASSERT_FALSE(notes);
  }
  TEST_ASSERT_EQUAL_UINT32((1UL << BOARD_REASON_COUNT) - 2, seen);
  TEST_ASSERT_EQUAL_STRING("?", boardReasonName((BoardReason)BOARD_REASON_COUNT));
}

static void test_counting_units_and_the_worse_of_two_levels() {
  UnitLevelCounts c;
  unitLevelCount(c, VerdictLevel::Working);
  unitLevelCount(c, VerdictLevel::Note);
  unitLevelCount(c, VerdictLevel::Fault);
  unitLevelCount(c, VerdictLevel::Fault);
  TEST_ASSERT_EQUAL_UINT8(2, c.fault);
  TEST_ASSERT_EQUAL_UINT8(1, c.note);
  TEST_ASSERT_TRUE(VerdictLevel::Fault == verdictWorse(VerdictLevel::Note, VerdictLevel::Fault));
  TEST_ASSERT_TRUE(VerdictLevel::Note == verdictWorse(VerdictLevel::Note, VerdictLevel::Working));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_a_healthy_board_is_working);
  RUN_TEST(test_a_lost_row_is_a_fault_and_judged_on_nothing_else);
  RUN_TEST(test_a_row_briefly_away_or_busy_is_no_fault);
  RUN_TEST(test_a_row_never_heard_is_waited_for_and_then_a_fault);
  RUN_TEST(test_a_row_taking_its_image_is_updating_even_while_gone);
  RUN_TEST(test_rescue_mode_is_a_fault);
  RUN_TEST(test_a_dead_bus_is_one_fault_not_one_per_unit);
  RUN_TEST(test_an_image_given_up_on_is_a_fault);
  RUN_TEST(test_units_missing_and_units_at_fault);
  RUN_TEST(test_more_units_than_placed_is_not_missing);
  RUN_TEST(test_the_notes);
  RUN_TEST(test_a_board_updating_its_units_is_not_judged_on_them);
  RUN_TEST(test_the_masters_own_row_is_judged_on_its_units_only);
  RUN_TEST(test_the_reason_numbers_and_names_are_fixed);
  RUN_TEST(test_every_reason_is_ranked_once_and_faults_come_first);
  RUN_TEST(test_counting_units_and_the_worse_of_two_levels);
  return UNITY_END();
}
