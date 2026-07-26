// Host-side tests for the follower's maintenance-op layer (#298): the
// {"seq":N} contract's validators (trimmed v2 MaintenancePolicy copy), the
// single-slot op/self-test result JSON (v2 DisplayIpc fragments), and the
// reflash progress object spliced into /units/health.

#include <ArduinoFake.h>
#include <unity.h>

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
  char buf[96];
  buildReflashJson(buf, sizeof(buf), p);
  TEST_ASSERT_EQUAL_STRING(
      "{\"state\":\"flashing\",\"total\":3,\"done\":1,\"failed\":0,\"cur\":2}",
      buf);
  reflashProgressFinish(p, false);
  TEST_ASSERT_FALSE(reflashInProgress(p));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_address_validation);
  RUN_TEST(test_protocol_mismatch_unit_is_409_not_drivable);
  RUN_TEST(test_offset_and_jog_ranges);
  RUN_TEST(test_op_result_pending_found_expired);
  RUN_TEST(test_op_result_failure_carries_reason);
  RUN_TEST(test_self_test_result_json);
  RUN_TEST(test_reflash_json_and_gate);
  return UNITY_END();
}
