// Native tests for UnitVerdict.h: what the master makes of one unit's facts.
#include <unity.h>

#include "../../UnitVerdict.h"

void setUp() {}
void tearDown() {}

// A unit with nothing to remark: answering, homed, current.
static UnitFacts healthy() {
  UnitFacts u;
  u.state = 1;
  u.fwStatus = 0;
  u.statusValid = true;
  u.status.flags = UNIT_FLAG_HOMED;
  u.status.uptimeSeconds = 600;
  u.bootVerdict = BOOT_INTEGRITY_OK;
  u.vitalsValid = true;
  u.vitals.vccMin_mV = 4900;
  return u;
}

static UnitVerdict judge(const UnitFacts& u, UnitVerdictContext ctx = UnitVerdictContext()) {
  return unitVerdict(u, ctx);
}

static bool has(const UnitVerdict& v, UnitReason r) { return (v.all & unitReasonBit(r)) != 0; }

static void test_a_healthy_unit_is_working_and_says_how_long() {
  const UnitVerdict v = judge(healthy());
  TEST_ASSERT_TRUE(VerdictLevel::Working == v.level);
  TEST_ASSERT_TRUE(UnitReason::Working == v.reason);
  TEST_ASSERT_EQUAL_UINT32(600, v.a);
  TEST_ASSERT_EQUAL_UINT32(0, v.all);
}

static void test_the_long_uptime_is_used_when_the_unit_reports_one() {
  UnitFacts u = healthy();
  u.status.uptimeSeconds = 0xFFFF;  // the short counter stops here
  u.linkValid = true;
  u.link.uptimeSeconds = 400000;
  TEST_ASSERT_EQUAL_UINT32(400000, judge(u).a);
}

static void test_a_place_without_a_unit_is_a_fault() {
  const UnitVerdict v = judge(UnitFacts());
  TEST_ASSERT_TRUE(VerdictLevel::Fault == v.level);
  TEST_ASSERT_TRUE(UnitReason::NoUnit == v.reason);
}

static void test_a_unit_that_stopped_answering_says_for_how_long() {
  UnitFacts u = healthy();
  u.stale = true;
  u.misses = 5;
  u.lastSeenMs = 1000;
  UnitVerdictContext ctx;
  ctx.nowMs = 91000;
  const UnitVerdict v = judge(u, ctx);
  TEST_ASSERT_TRUE(VerdictLevel::Fault == v.level);
  TEST_ASSERT_TRUE(UnitReason::NotAnswering == v.reason);
  TEST_ASSERT_EQUAL_UINT32(90, v.a);
  TEST_ASSERT_EQUAL_UINT32(5, v.b);
}

static void test_a_silent_unit_is_judged_on_nothing_else() {
  // Its last status is from before it went quiet: not evidence of anything now.
  UnitFacts u = healthy();
  u.stale = true;
  u.status.flags |= UNIT_FLAG_LAST_HOME_FAILED;
  u.fwStatus = 1;
  const UnitVerdict v = judge(u);
  TEST_ASSERT_EQUAL_UINT32(unitReasonBit(UnitReason::NotAnswering), v.all);
}

static void test_a_unit_in_its_bootloader_is_a_fault_unless_the_board_is_updating() {
  UnitFacts u;
  u.state = 2;
  UnitVerdict v = judge(u);
  TEST_ASSERT_TRUE(VerdictLevel::Fault == v.level);
  TEST_ASSERT_TRUE(UnitReason::InBootloader == v.reason);
  UnitVerdictContext ctx;
  ctx.updating = true;
  v = judge(u, ctx);
  TEST_ASSERT_TRUE(VerdictLevel::Note == v.level);
  TEST_ASSERT_TRUE(UnitReason::BeingUpdated == v.reason);
}

static void test_a_unit_held_for_crashing_is_a_fault_even_during_an_update() {
  UnitFacts u;
  u.state = 2;
  u.bootloader.crashValid = true;
  u.bootloader.crashCount = TWIBOOT_CRASH_HOLD_COUNT;
  UnitVerdictContext ctx;
  ctx.updating = true;
  const UnitVerdict v = judge(u, ctx);
  TEST_ASSERT_TRUE(VerdictLevel::Fault == v.level);
  TEST_ASSERT_TRUE(UnitReason::HeldInBootloader == v.reason);
  TEST_ASSERT_EQUAL_UINT32(TWIBOOT_CRASH_HOLD_COUNT, v.a);
}

static void test_a_unit_speaking_another_contract_is_a_fault() {
  UnitFacts u = healthy();
  u.protocolKnown = true;
  u.protocolVersion = (uint8_t)(SFP_PROTOCOL_VERSION + 1);
  const UnitVerdict v = judge(u);
  TEST_ASSERT_TRUE(UnitReason::WrongProtocol == v.reason);
  TEST_ASSERT_EQUAL_UINT32(SFP_PROTOCOL_VERSION + 1, v.a);
}

static void test_a_damaged_bootloader_is_a_fault_and_an_old_one_a_note() {
  UnitFacts u = healthy();
  u.bootVerdict = BOOT_INTEGRITY_CORRUPT;
  u.bootCrc32 = 0xDEADBEEF;
  UnitVerdict v = judge(u);
  TEST_ASSERT_TRUE(VerdictLevel::Fault == v.level);
  TEST_ASSERT_TRUE(UnitReason::BootloaderDamaged == v.reason);
  TEST_ASSERT_EQUAL_HEX32(0xDEADBEEF, v.a);
  u.bootVerdict = BOOT_INTEGRITY_OUTDATED;
  v = judge(u);
  TEST_ASSERT_TRUE(VerdictLevel::Note == v.level);
  TEST_ASSERT_TRUE(UnitReason::BootloaderOutdated == v.reason);
}

static void test_a_failed_home_and_a_dead_hall_sensor_are_faults() {
  UnitFacts u = healthy();
  u.status.flags |= UNIT_FLAG_LAST_HOME_FAILED;
  u.lifetimeValid = true;
  u.lifetime.homeFailedCount = 3;
  UnitVerdict v = judge(u);
  TEST_ASSERT_TRUE(VerdictLevel::Fault == v.level);
  TEST_ASSERT_TRUE(UnitReason::HomeFailed == v.reason);
  TEST_ASSERT_EQUAL_UINT32(3, v.a);
  u.status.flags |= UNIT_FLAG_HALL_NEVER;
  v = judge(u);
  TEST_ASSERT_TRUE(UnitReason::HallNever == v.reason);
  TEST_ASSERT_TRUE(has(v, UnitReason::HomeFailed));
}

static void test_a_unit_that_has_not_homed_yet_is_a_note() {
  UnitFacts u = healthy();
  u.status.flags = UNIT_FLAG_MOVING;
  const UnitVerdict v = judge(u);
  TEST_ASSERT_TRUE(VerdictLevel::Note == v.level);
  TEST_ASSERT_TRUE(UnitReason::FindingHome == v.reason);
  TEST_ASSERT_EQUAL_UINT32(1, v.a);
}

static void test_a_restart_by_itself_is_a_note_not_a_fault() {
  UnitFacts u = healthy();
  u.resetSeen = true;
  u.status.lifetimeBrownoutCount = 4;
  u.status.lifetimeWatchdogCount = 1;
  const UnitVerdict v = judge(u);
  TEST_ASSERT_TRUE(VerdictLevel::Note == v.level);
  TEST_ASSERT_TRUE(UnitReason::RestartedByItself == v.reason);
  TEST_ASSERT_EQUAL_UINT32(4, v.a);
  TEST_ASSERT_EQUAL_UINT32(1, v.b);
}

static void test_low_supply_carries_the_reading_and_the_floor() {
  UnitFacts u = healthy();
  u.vitals.vccMin_mV = 3890;
  const UnitVerdict v = judge(u);
  TEST_ASSERT_TRUE(UnitReason::LowSupply == v.reason);
  TEST_ASSERT_EQUAL_UINT32(3890, v.a);
  TEST_ASSERT_EQUAL_UINT32(UNIT_VCC_MIN_FLOOR_MV, v.b);
  u.vitals.vccMin_mV = 0;  // no reading
  TEST_ASSERT_TRUE(UnitReason::Working == judge(u).reason);
}

static void test_the_drum_measurements_are_notes() {
  UnitFacts u = healthy();
  u.extDiagValid = true;
  u.extDiag.statusBits = EXT_DIAG_STATUS_STALL;
  u.extDiag.stepExcessMax = EXT_DIAG_DRAG_EXCESS_STEPS + 1;
  u.extDiag.hallEdgesLastRev = 2;
  const UnitVerdict v = judge(u);
  TEST_ASSERT_TRUE(VerdictLevel::Note == v.level);
  TEST_ASSERT_TRUE(UnitReason::Jammed == v.reason);
  TEST_ASSERT_TRUE(has(v, UnitReason::Dragging));
  TEST_ASSERT_TRUE(has(v, UnitReason::HallAnomaly));
  // No completed turn measured is not an anomaly (#418).
  u = healthy();
  u.extDiagValid = true;
  u.extDiag.hallEdgesLastRev = 0;
  TEST_ASSERT_EQUAL_UINT32(0, judge(u).all);
}

static void test_the_remaining_notes() {
  UnitFacts u = healthy();
  u.fwStatus = 1;
  TEST_ASSERT_TRUE(UnitReason::FirmwareOutdated == judge(u).reason);
  u = healthy();
  u.mismatch = true;
  u.diagValid = true;
  u.physLetter = 7;
  UnitVerdict v = judge(u);
  TEST_ASSERT_TRUE(UnitReason::WrongLetter == v.reason);
  TEST_ASSERT_EQUAL_UINT32(7, v.a);
  u = healthy();
  u.odometerValid = true;
  u.odometer = 30000;
  UnitVerdictContext ctx;
  ctx.worn = true;
  v = judge(u, ctx);
  TEST_ASSERT_TRUE(UnitReason::Worn == v.reason);
  TEST_ASSERT_EQUAL_UINT32(30000, v.a);
  u = healthy();
  u.statusValid = false;
  TEST_ASSERT_TRUE(UnitReason::NotRead == judge(u).reason);
}

static void test_a_home_failure_in_the_past_is_the_weakest_note() {
  UnitFacts u = healthy();
  u.lifetimeValid = true;
  u.lifetime.homeFailedCount = 10;
  UnitVerdict v = judge(u);
  TEST_ASSERT_TRUE(VerdictLevel::Note == v.level);
  TEST_ASSERT_TRUE(UnitReason::HomeFailedBefore == v.reason);
  TEST_ASSERT_EQUAL_UINT32(10, v.a);
  TEST_ASSERT_EQUAL_UINT32(600, v.b);
  u.fwStatus = 1;
  v = judge(u);
  TEST_ASSERT_TRUE(UnitReason::FirmwareOutdated == v.reason);
  TEST_ASSERT_TRUE(has(v, UnitReason::HomeFailedBefore));
}

static void test_a_fault_leads_over_any_note() {
  UnitFacts u = healthy();
  u.fwStatus = 1;
  u.resetSeen = true;
  u.status.flags |= UNIT_FLAG_LAST_HOME_FAILED;
  const UnitVerdict v = judge(u);
  TEST_ASSERT_TRUE(VerdictLevel::Fault == v.level);
  TEST_ASSERT_TRUE(UnitReason::HomeFailed == v.reason);
  TEST_ASSERT_TRUE(has(v, UnitReason::FirmwareOutdated));
  TEST_ASSERT_TRUE(has(v, UnitReason::RestartedByItself));
}

// The numbers are stored in the event record and the names are the API's:
// neither may move. A new reason goes at the end.
static void test_the_reason_numbers_and_names_are_fixed() {
  static const struct { UnitReason r; uint8_t n; const char* name; } fixed[] = {
      {UnitReason::Working, 0, "working"},
      {UnitReason::NoUnit, 1, "no-unit"},
      {UnitReason::NotAnswering, 2, "not-answering"},
      {UnitReason::HeldInBootloader, 3, "held-in-bootloader"},
      {UnitReason::InBootloader, 4, "in-bootloader"},
      {UnitReason::WrongProtocol, 5, "wrong-protocol"},
      {UnitReason::BootloaderDamaged, 6, "bootloader-damaged"},
      {UnitReason::HomeFailed, 7, "home-failed"},
      {UnitReason::HallNever, 8, "hall-never"},
      {UnitReason::BeingUpdated, 9, "being-updated"},
      {UnitReason::FindingHome, 10, "finding-home"},
      {UnitReason::Jammed, 11, "jammed"},
      {UnitReason::WrongLetter, 12, "wrong-letter"},
      {UnitReason::LowSupply, 13, "low-supply"},
      {UnitReason::RestartedByItself, 14, "restarted-by-itself"},
      {UnitReason::FirmwareOutdated, 15, "firmware-outdated"},
      {UnitReason::BootloaderOutdated, 16, "bootloader-outdated"},
      {UnitReason::Dragging, 17, "dragging"},
      {UnitReason::HallAnomaly, 18, "hall-anomaly"},
      {UnitReason::Worn, 19, "worn"},
      {UnitReason::NotRead, 20, "not-read"},
      {UnitReason::HomeFailedBefore, 21, "home-failed-before"},
  };
  TEST_ASSERT_EQUAL(UNIT_REASON_COUNT, (int)(sizeof(fixed) / sizeof(fixed[0])));
  for (const auto& f : fixed) {
    TEST_ASSERT_EQUAL_UINT8(f.n, (uint8_t)f.r);
    TEST_ASSERT_EQUAL_STRING(f.name, unitReasonName(f.r));
  }
}

// Every reason has its place in the order they lead in, once, faults first.
static void test_every_reason_is_ranked_once_and_faults_come_first() {
  uint32_t seen = 0;
  bool notes = false;
  for (UnitReason r : UNIT_REASON_ORDER) {
    TEST_ASSERT_TRUE(r != UnitReason::Working);
    TEST_ASSERT_EQUAL_UINT32(0, seen & unitReasonBit(r));
    seen |= unitReasonBit(r);
    if (unitReasonLevel(r) == VerdictLevel::Note) notes = true;
    else TEST_ASSERT_FALSE(notes);
  }
  TEST_ASSERT_EQUAL_UINT32((1UL << UNIT_REASON_COUNT) - 2, seen);
  TEST_ASSERT_TRUE(VerdictLevel::Working == unitReasonLevel(UnitReason::Working));
  TEST_ASSERT_EQUAL_STRING("?", unitReasonName((UnitReason)UNIT_REASON_COUNT));
}

static void test_the_level_names() {
  TEST_ASSERT_EQUAL_STRING("working", verdictLevelName(VerdictLevel::Working));
  TEST_ASSERT_EQUAL_STRING("note", verdictLevelName(VerdictLevel::Note));
  TEST_ASSERT_EQUAL_STRING("fault", verdictLevelName(VerdictLevel::Fault));
}

static void test_the_fault_count_counts_faults_only() {
  UnitVerdict units[4];
  units[0].level = VerdictLevel::Fault;
  units[1].level = VerdictLevel::Note;
  units[2].level = VerdictLevel::Working;
  units[3].level = VerdictLevel::Fault;
  TEST_ASSERT_EQUAL(2, unitFaultCount(units, 4));
  TEST_ASSERT_EQUAL(1, unitFaultCount(units, 3));
  TEST_ASSERT_EQUAL(0, unitFaultCount(units, 0));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_a_healthy_unit_is_working_and_says_how_long);
  RUN_TEST(test_the_long_uptime_is_used_when_the_unit_reports_one);
  RUN_TEST(test_a_place_without_a_unit_is_a_fault);
  RUN_TEST(test_a_unit_that_stopped_answering_says_for_how_long);
  RUN_TEST(test_a_silent_unit_is_judged_on_nothing_else);
  RUN_TEST(test_a_unit_in_its_bootloader_is_a_fault_unless_the_board_is_updating);
  RUN_TEST(test_a_unit_held_for_crashing_is_a_fault_even_during_an_update);
  RUN_TEST(test_a_unit_speaking_another_contract_is_a_fault);
  RUN_TEST(test_a_damaged_bootloader_is_a_fault_and_an_old_one_a_note);
  RUN_TEST(test_a_failed_home_and_a_dead_hall_sensor_are_faults);
  RUN_TEST(test_a_unit_that_has_not_homed_yet_is_a_note);
  RUN_TEST(test_a_restart_by_itself_is_a_note_not_a_fault);
  RUN_TEST(test_low_supply_carries_the_reading_and_the_floor);
  RUN_TEST(test_the_drum_measurements_are_notes);
  RUN_TEST(test_the_remaining_notes);
  RUN_TEST(test_a_home_failure_in_the_past_is_the_weakest_note);
  RUN_TEST(test_a_fault_leads_over_any_note);
  RUN_TEST(test_the_reason_numbers_and_names_are_fixed);
  RUN_TEST(test_every_reason_is_ranked_once_and_faults_come_first);
  RUN_TEST(test_the_level_names);
  RUN_TEST(test_the_fault_count_counts_faults_only);
  return UNITY_END();
}
