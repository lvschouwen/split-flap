// Host-side tests for the follower's maintenance-op layer (#298): the
// {"seq":N} contract's validators (trimmed v2 MaintenancePolicy copy), the
// single-slot op/self-test result JSON (v2 DisplayIpc fragments), and the
// reflash progress object spliced into /units/health.

#include <ArduinoFake.h>
#include <unity.h>

#include <string.h>
#include "../../FollowerScanLog.h"
#include "../../FollowerOps.h"

void setUp() {}
void tearDown() {}

// --- validators -------------------------------------------------------------------

static void test_address_validation() {
  UnitFacts units[16];
  units[1].state = 1;  // sketch-running unit at address 2
  int addr = 0;
  TEST_ASSERT_EQUAL(400, maintValidateAddress(nullptr, units, 16, addr).httpStatus);
  TEST_ASSERT_EQUAL(400, maintValidateAddress("abc", units, 16, addr).httpStatus);
  TEST_ASSERT_EQUAL(400, maintValidateAddress("0", units, 16, addr).httpStatus);
  TEST_ASSERT_EQUAL(404, maintValidateAddress("5", units, 16, addr).httpStatus);
  TEST_ASSERT_EQUAL(200, maintValidateAddress("2", units, 16, addr).httpStatus);
  TEST_ASSERT_EQUAL(2, addr);
}

static void test_protocol_mismatch_unit_is_409_not_drivable() {
  // #405 gap found in review: this gate checked only state==1, so a unit
  // speaking a contract we have no code for would still receive every
  // single-unit op — including the unverifiable SET_I2C_ADDRESS burn. Both
  // rows must refuse identically.
  UnitFacts units[16];
  units[1].state = 1;
  units[1].protocolKnown = true;
  units[1].protocolVersion = SFP_PROTOCOL_VERSION;
  int addr = 0;
  TEST_ASSERT_EQUAL(200, maintValidateAddress("2", units, 16, addr).httpStatus);
  units[1].protocolVersion = (uint8_t)(SFP_PROTOCOL_VERSION + 1);
  TEST_ASSERT_EQUAL(409, maintValidateAddress("2", units, 16, addr).httpStatus);
  // Unreadable is absence of evidence, not evidence of difference.
  units[1].protocolKnown = false;
  TEST_ASSERT_EQUAL(200, maintValidateAddress("2", units, 16, addr).httpStatus);
}

static void test_offset_and_jog_ranges() {
  TEST_ASSERT_EQUAL(200, maintValidateOffset(SFP_OFFSET_LIMIT_STEPS).httpStatus);
  TEST_ASSERT_EQUAL(400, maintValidateOffset(SFP_OFFSET_LIMIT_STEPS + 1).httpStatus);
  TEST_ASSERT_EQUAL(200, maintValidateJog(-127).httpStatus);
  TEST_ASSERT_EQUAL(400, maintValidateJog(128).httpStatus);
}

// Feature gates ride the wire as one byte (#409). The vocabulary check is NOT
// the unit's after all (#458): UNIT_GATE_ALL admits 0x02, which nothing
// implements, and the GET_LIFETIME read-back confirms it as active because it
// sees storage rather than behaviour. This row must reject exactly what the
// S3 rejects — a permissive follower would just be the same hole, one row
// over.
static void test_gates_reject_bits_no_firmware_implements() {
  TEST_ASSERT_EQUAL(200, maintValidateGates(0).httpStatus);
  TEST_ASSERT_EQUAL(200, maintValidateGates(SFP_UNIT_GATE_IDLE_HALL_CHECK).httpStatus);
  TEST_ASSERT_EQUAL(400, maintValidateGates(0x02).httpStatus);
  TEST_ASSERT_EQUAL(400, maintValidateGates(0x03).httpStatus);
  TEST_ASSERT_EQUAL(400, maintValidateGates(255).httpStatus);
  TEST_ASSERT_EQUAL(400, maintValidateGates(256).httpStatus);
  TEST_ASSERT_EQUAL(400, maintValidateGates(-1).httpStatus);
}

// --- op-result slot ----------------------------------------------------------------

static void test_op_result_pending_found_expired() {
  MaintResult slot;
  char buf[96];
  buildOpResultJson(buf, sizeof(buf), slot, 1);
  TEST_ASSERT_EQUAL_STRING("{\"state\":\"pending\"}", buf);

  slot.seq = 1;
  slot.outcome = MaintOutcome::Ok;
  buildOpResultJson(buf, sizeof(buf), slot, 1);
  TEST_ASSERT_EQUAL_STRING("{\"state\":\"ok\"}", buf);

  buildOpResultJson(buf, sizeof(buf), slot, 2);  // newer query than the slot
  TEST_ASSERT_EQUAL_STRING("{\"state\":\"pending\"}", buf);

  slot.seq = 5;
  buildOpResultJson(buf, sizeof(buf), slot, 2);  // slot moved past this seq
  TEST_ASSERT_EQUAL_STRING("{\"state\":\"expired\"}", buf);
}

static void test_op_result_failure_carries_reason() {
  MaintResult slot;
  slot.seq = 3;
  slot.outcome = MaintOutcome::WireFail;
  char buf[96];
  buildOpResultJson(buf, sizeof(buf), slot, 3);
  TEST_ASSERT_EQUAL_STRING("{\"state\":\"failed\",\"reason\":\"wire-fail\"}",
                           buf);
}

// --- self-test slot ----------------------------------------------------------------

static void test_self_test_result_json() {
  SelfTestSlot slot;
  char buf[128];
  buildSelfTestJson(buf, sizeof(buf), slot, 1);
  TEST_ASSERT_EQUAL_STRING("{\"state\":\"pending\"}", buf);

  slot.seq = 1;
  slot.outcome = SelfTestOutcome::Ok;
  slot.stepsPerRev = 2050;
  slot.hallWindowSteps = 59;
  slot.revTimeMs = 4100;
  buildSelfTestJson(buf, sizeof(buf), slot, 1);
  TEST_ASSERT_EQUAL_STRING(
      "{\"state\":\"ok\",\"steps_per_rev\":2050,\"hall_window\":59,"
      "\"rev_time_ms\":4100}",
      buf);

  // #404: a failure carries the unit's own failure mode plus whatever it
  // measured before giving up — byte-for-byte the master's shape, so both
  // rows report identically.
  slot.outcome = SelfTestOutcome::Timeout;
  buildSelfTestJson(buf, sizeof(buf), slot, 1);
  TEST_ASSERT_EQUAL_STRING(
      "{\"state\":\"failed\",\"reason\":\"timeout\",\"unit_reason\":\"none\","
      "\"steps_per_rev\":2050,\"hall_window\":59,\"rev_time_ms\":4100}",
      buf);

  slot.outcome = SelfTestOutcome::UnitFailed;
  slot.unitReason = SELFTEST_REASON_HALL_NEVER;
  buildSelfTestJson(buf, sizeof(buf), slot, 1);
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"unit_reason\":\"hall-never\""));
}

// --- reflash progress ---------------------------------------------------------------

static void test_reflash_json_and_gate() {
  ReflashProgress p;
  TEST_ASSERT_FALSE(reflashInProgress(p));
  reflashProgressBegin(p, 3);
  TEST_ASSERT_TRUE(reflashInProgress(p));
  reflashProgressUnitStart(p, 2);
  reflashProgressUnitResult(p, true);
  char buf[REFLASH_JSON_CAP];
  buildReflashJson(buf, sizeof(buf), p);
  TEST_ASSERT_EQUAL_STRING(
      "{\"state\":\"flashing\",\"total\":3,\"done\":1,\"failed\":0,\"cur\":2,"
      "\"halted\":false,\"boot\":0,\"bootFailed\":0}",
      buf);
  reflashProgressFinish(p, false, false);
  TEST_ASSERT_FALSE(reflashInProgress(p));
}

// The ESP-01 row runs the shared plan (#527): two failures back to back stop
// the sweep and the progress object says so; one success in between does not.
static void test_reflash_halts_on_consecutive_failures() {
  TEST_ASSERT_FALSE(reflashShouldHalt(1));
  TEST_ASSERT_TRUE(reflashShouldHalt(REFLASH_MAX_CONSECUTIVE_FAILURES));

  ReflashProgress p;
  reflashProgressBegin(p, 5);
  reflashProgressUnitResult(p, false);
  reflashProgressUnitResult(p, false);
  reflashProgressFinish(p, false, true);
  TEST_ASSERT_TRUE(p.halted);
  char buf[96];
  buildReflashJson(buf, sizeof(buf), p);
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"state\":\"failed\""));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"halted\":true"));

  reflashProgressBegin(p, 5);  // a new run clears the flag
  TEST_ASSERT_FALSE(p.halted);
}

// #405 on this row too: a unit that answers with a protocol version we do not
// speak is a reflash target even when its rev reads current.
static void test_protocol_mismatch_unit_is_a_reflash_target() {
  UnitFacts facts[2];
  facts[0].state = 1;
  facts[0].fwStatus = 0;
  facts[0].protocolKnown = true;
  facts[0].protocolVersion = (uint8_t)(SFP_PROTOCOL_VERSION + 1);
  facts[1].state = 1;
  facts[1].fwStatus = 0;
  facts[1].protocolKnown = true;
  facts[1].protocolVersion = SFP_PROTOCOL_VERSION;
  uint8_t addrs[2];
  TEST_ASSERT_EQUAL_INT(1, reflashCollectRebootTargets(facts, 2, 1, addrs));
  TEST_ASSERT_EQUAL_UINT8(1, addrs[0]);
  TEST_ASSERT_EQUAL_INT(1, reflashCollectOutdatedTargets(facts, 2, 1, addrs));
  TEST_ASSERT_EQUAL_UINT8(1, addrs[0]);
}

// The worst-case progress object must fit the buffer both web layers hand it.
static void test_reflash_json_worst_case_fits() {
  ReflashProgress p;
  p.state = ReflashState::Cancelled;
  p.total = p.done = p.failed = p.currentAddr = 255;
  char buf[REFLASH_JSON_CAP];
  buildReflashJson(buf, sizeof(buf), p);
  TEST_ASSERT_EQUAL_CHAR('}', buf[strlen(buf) - 1]);
}

// --- single-unit reflash (#513) ---

static void test_filter_zero_means_no_filter() {
  uint8_t addrs[4] = {1, 2, 3, 4};
  TEST_ASSERT_EQUAL_INT(4, reflashFilterToAddress(addrs, 4, 0));
  TEST_ASSERT_EQUAL_UINT8(1, addrs[0]);
  TEST_ASSERT_EQUAL_UINT8(4, addrs[3]);
}

static void test_filter_keeps_only_the_target() {
  uint8_t addrs[4] = {1, 2, 3, 4};
  TEST_ASSERT_EQUAL_INT(1, reflashFilterToAddress(addrs, 4, 3));
  TEST_ASSERT_EQUAL_UINT8(3, addrs[0]);
}

static void test_filter_target_not_collected_plans_nothing() {
  // The unit is current, silent, or stranded at another address: a run aimed
  // at it must not fall back to flashing its neighbours.
  uint8_t addrs[3] = {1, 2, 4};
  TEST_ASSERT_EQUAL_INT(0, reflashFilterToAddress(addrs, 3, 3));
  TEST_ASSERT_EQUAL_INT(0, reflashFilterToAddress(addrs, 0, 3));
}

static void test_reflash_address_range() {
  TEST_ASSERT_FALSE(reflashAddressInRange(0, 1, 16));  // general call
  TEST_ASSERT_TRUE(reflashAddressInRange(1, 1, 16));
  TEST_ASSERT_TRUE(reflashAddressInRange(16, 1, 16));
  TEST_ASSERT_FALSE(reflashAddressInRange(17, 1, 16));
  TEST_ASSERT_FALSE(reflashAddressInRange(-1, 1, 16));
  TEST_ASSERT_FALSE(reflashAddressInRange(300, 1, 16));
}

static void test_reflash_address_parses_decimal_only() {
  long v = -1;
  TEST_ASSERT_TRUE(reflashParseAddress("3", v));
  TEST_ASSERT_EQUAL_INT(3, (int)v);
  TEST_ASSERT_TRUE(reflashParseAddress("16", v));
  TEST_ASSERT_EQUAL_INT(16, (int)v);
  TEST_ASSERT_TRUE(reflashParseAddress("0", v));
  TEST_ASSERT_EQUAL_INT(0, (int)v);  // parses; the range check refuses it
  v = 99;
  const char* bad[] = {"",    "010", "0x3", "3abc", " 3",  "3 ",
                       "-3",  "+3",  "3.0", "1234", "abc", "03"};
  for (const char* b : bad) TEST_ASSERT_FALSE(reflashParseAddress(b, v));
  TEST_ASSERT_FALSE(reflashParseAddress(nullptr, v));
  TEST_ASSERT_EQUAL_INT(99, (int)v);  // untouched on rejection
}

// #516: same contract as the S3 — an ok can carry a reason, and a refusal by
// the unit has its own name.
static void test_op_result_ok_carries_its_reason() {
  MaintResult slot;
  slot.seq = 4;
  slot.outcome = MaintOutcome::Ok;
  slot.reason = MaintReason::BootAlreadyNew;
  char buf[96];
  buildOpResultJson(buf, sizeof(buf), slot, 4);
  TEST_ASSERT_EQUAL_STRING("{\"state\":\"ok\",\"detail\":\"boot-already-new\"}",
                           buf);
  slot.reason = MaintReason::None;
  buildOpResultJson(buf, sizeof(buf), slot, 4);
  TEST_ASSERT_EQUAL_STRING("{\"state\":\"ok\"}", buf);
  slot.outcome = MaintOutcome::PostconditionFail;
  slot.reason = MaintReason::BootUnitBusy;
  buildOpResultJson(buf, sizeof(buf), slot, 4);
  TEST_ASSERT_EQUAL_STRING(
      "{\"state\":\"failed\",\"reason\":\"postcondition-fail\","
      "\"detail\":\"boot-unit-busy\"}",
      buf);
}

static void test_boot_reasons_have_distinct_names() {
  const MaintReason reasons[] = {
      MaintReason::BootInfoReadFail, MaintReason::BootStateUnknown,
      MaintReason::BootLockRefused,  MaintReason::BootUnitLost,
      MaintReason::BootVerifyFailed, MaintReason::BootAlreadyNew,
      MaintReason::BootUnitBusy,     MaintReason::BootNotStarted};
  const int n = (int)(sizeof(reasons) / sizeof(reasons[0]));
  for (int i = 0; i < n; i++) {
    TEST_ASSERT_TRUE(strlen(maintReasonName(reasons[i])) > 5);
    for (int j = i + 1; j < n; j++) {
      TEST_ASSERT_TRUE(strcmp(maintReasonName(reasons[i]),
                              maintReasonName(reasons[j])) != 0);
    }
  }
}

// #516: the unit's own failure names the reason; when it reported none, the
// caller's fallback is used — never a guess.
static void test_boot_failure_maps_to_its_own_reason() {
  const MaintReason fb = MaintReason::BootNotStarted;
  TEST_ASSERT_TRUE(maintReasonForBootFailure(BOOT_FAIL_BUSY, fb) == MaintReason::BootUnitBusy);
  TEST_ASSERT_TRUE(maintReasonForBootFailure(BOOT_FAIL_LOCK, fb) == MaintReason::BootLockRefused);
  TEST_ASSERT_TRUE(maintReasonForBootFailure(BOOT_FAIL_STATE, fb) == MaintReason::BootStateUnknown);
  TEST_ASSERT_TRUE(maintReasonForBootFailure(BOOT_FAIL_VERIFY, fb) == MaintReason::BootVerifyFailed);
  TEST_ASSERT_TRUE(maintReasonForBootFailure(BOOT_FAIL_NONE, fb) == MaintReason::BootNotStarted);
  TEST_ASSERT_TRUE(maintReasonForBootFailure(BOOT_FAIL_NONE, MaintReason::BootVerifyFailed) ==
                   MaintReason::BootVerifyFailed);
  TEST_ASSERT_EQUAL_STRING("boot-not-started",
                           maintReasonName(MaintReason::BootNotStarted));
}

// --- scan log: one line per unit that is not current (#549) -----------------

static UnitFacts scanFact(uint8_t state, uint8_t fwStatus, const char* rev) {
  UnitFacts f{};
  f.state = state;
  f.fwStatus = fwStatus;
  strncpy(f.version, rev, sizeof(f.version) - 1);
  return f;
}

static void test_scan_line_is_silent_for_a_current_unit_and_an_empty_slot() {
  char buf[FOLLOWER_SCAN_LINE_CAP] = "untouched";
  TEST_ASSERT_EQUAL_UINT8(SCAN_FINDING_NONE, followerScanLine(buf, sizeof(buf), 1,
                                     scanFact(1, 0, "1089153"), 1));
  TEST_ASSERT_EQUAL_UINT8(SCAN_FINDING_NONE, followerScanLine(buf, sizeof(buf), 1,
                                     scanFact(1, 0, "1089153"), 0));
  TEST_ASSERT_EQUAL_UINT8(SCAN_FINDING_NONE, followerScanLine(buf, sizeof(buf), 6,
                                     scanFact(0, 2, ""), 0));
  TEST_ASSERT_EQUAL_STRING("untouched", buf);
}

static void test_scan_line_names_each_state_that_needs_attention() {
  char buf[FOLLOWER_SCAN_LINE_CAP];
  TEST_ASSERT_EQUAL_UINT8(SCAN_FINDING_OUTDATED, followerScanLine(buf, sizeof(buf), 1,
                                    scanFact(1, 1, "cfd9948"), 1));
  TEST_ASSERT_EQUAL_STRING(
      "- unit at 0x01 is running sketch (fw cfd9948 — OUTDATED)", buf);

  TEST_ASSERT_EQUAL_UINT8(SCAN_FINDING_UNREADABLE, followerScanLine(buf, sizeof(buf), 2, scanFact(1, 2, ""), 1));
  TEST_ASSERT_EQUAL_STRING(
      "- unit at 0x02 is running sketch (fw UNKNOWN — unreadable version "
      "reply)", buf);

  TEST_ASSERT_EQUAL_UINT8(SCAN_FINDING_BOOTLOADER, followerScanLine(buf, sizeof(buf), 3, scanFact(2, 2, ""), 1));
  TEST_ASSERT_EQUAL_STRING("- unit at 0x03 is in BOOTLOADER mode", buf);
  UnitFacts named = scanFact(2, 2, "");
  named.bootloader.generation = 2;
  named.bootloader.fusesValid = true;
  named.bootloader.lock = 0xCF;
  named.bootloader.lfuse = 0xFF;
  named.bootloader.hfuse = 0xDA;
  named.bootloader.efuse = 0xFD;
  TEST_ASSERT_EQUAL_UINT8(SCAN_FINDING_BOOTLOADER,
                          followerScanLine(buf, sizeof(buf), 3, named, 1));
  TEST_ASSERT_EQUAL_STRING(
      "- unit at 0x03 is in BOOTLOADER mode (bootloader v2, lock cf, fuses l "
      "ff h da e fd)", buf);
  TEST_ASSERT_TRUE(strlen(buf) < FOLLOWER_SCAN_LINE_CAP - 1);

  TEST_ASSERT_EQUAL_UINT8(SCAN_FINDING_MISSING, followerScanLine(buf, sizeof(buf), 4, scanFact(0, 2, ""), 1));
  TEST_ASSERT_EQUAL_STRING(
      "- unit at 0x04 is MISSING (answered the previous scan)", buf);
  TEST_ASSERT_EQUAL_UINT8(SCAN_FINDING_MISSING, followerScanLine(buf, sizeof(buf), 4, scanFact(0, 2, ""), 2));
}

static void test_scan_line_names_a_protocol_it_cannot_drive() {
  UnitFacts f = scanFact(1, 0, "abcdef0");
  f.protocolKnown = true;
  f.protocolVersion = (uint8_t)(SFP_PROTOCOL_VERSION + 1);
  char buf[FOLLOWER_SCAN_LINE_CAP];
  TEST_ASSERT_EQUAL_UINT8(SCAN_FINDING_PROTOCOL, followerScanLine(buf, sizeof(buf), 16, f, 1));
  char want[FOLLOWER_SCAN_LINE_CAP];
  snprintf(want, sizeof(want),
           "- unit at 0x10 speaks protocol v%u, we speak v%u — NOT DRIVABLE, "
           "reflash target",
           (unsigned)(SFP_PROTOCOL_VERSION + 1), (unsigned)SFP_PROTOCOL_VERSION);
  TEST_ASSERT_EQUAL_STRING(want, buf);
  // The widest line fits the buffer with room to spare.
  TEST_ASSERT_TRUE(strlen(buf) < FOLLOWER_SCAN_LINE_CAP - 1);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_address_validation);
  RUN_TEST(test_protocol_mismatch_unit_is_409_not_drivable);
  RUN_TEST(test_offset_and_jog_ranges);
  RUN_TEST(test_gates_reject_bits_no_firmware_implements);
  RUN_TEST(test_op_result_pending_found_expired);
  RUN_TEST(test_op_result_failure_carries_reason);
  RUN_TEST(test_self_test_result_json);
  RUN_TEST(test_reflash_json_and_gate);
  RUN_TEST(test_reflash_halts_on_consecutive_failures);
  RUN_TEST(test_protocol_mismatch_unit_is_a_reflash_target);
  RUN_TEST(test_reflash_json_worst_case_fits);
  RUN_TEST(test_filter_zero_means_no_filter);
  RUN_TEST(test_filter_keeps_only_the_target);
  RUN_TEST(test_filter_target_not_collected_plans_nothing);
  RUN_TEST(test_reflash_address_range);
  RUN_TEST(test_reflash_address_parses_decimal_only);
  RUN_TEST(test_op_result_ok_carries_its_reason);
  RUN_TEST(test_boot_reasons_have_distinct_names);
  RUN_TEST(test_boot_failure_maps_to_its_own_reason);
  RUN_TEST(test_scan_line_is_silent_for_a_current_unit_and_an_empty_slot);
  RUN_TEST(test_scan_line_names_each_state_that_needs_attention);
  RUN_TEST(test_scan_line_names_a_protocol_it_cannot_drive);
  return UNITY_END();
}
