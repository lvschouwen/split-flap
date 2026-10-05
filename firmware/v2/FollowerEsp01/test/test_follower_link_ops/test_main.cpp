// Native tests for FollowerLinkOps.h: which unit job a wall link Op becomes
// on this row, and why one is refused.
#include <unity.h>

#include "BootDump.h"
#include "FollowerLinkOps.h"

static UnitFacts units[UNITS_AMOUNT];

void setUp() {
  for (auto& u : units) u = UnitFacts{};
  units[0].state = 1;  // address 1: running
  units[1].state = 2;  // address 2: in its bootloader
  units[2].state = 1;  // address 3: running a protocol this build does not speak
  units[2].protocolKnown = true;
  units[2].protocolVersion = 250;
}
void tearDown() {}

static FollowerLinkOpPlan plan(uint32_t opcode, uint32_t address, long arg = 0) {
  return followerLinkPlanOp(opcode, address, arg, units, UNITS_AMOUNT);
}

static void test_every_job_code_has_its_op() {
  struct { wl_OpCode code; FollowerOpKind kind; } table[] = {
      {wl_OpCode_OPC_HOME, FollowerOpKind::Home},
      {wl_OpCode_OPC_IDENTIFY, FollowerOpKind::Identify},
      {wl_OpCode_OPC_JOG, FollowerOpKind::Jog},
      {wl_OpCode_OPC_SET_OFFSET, FollowerOpKind::WriteOffset},
      {wl_OpCode_OPC_SELF_TEST, FollowerOpKind::SelfTest},
      {wl_OpCode_OPC_RESTART_UNIT, FollowerOpKind::RebootToBootloader},
      {wl_OpCode_OPC_RESET_ODOMETER, FollowerOpKind::ResetOdometer},
      {wl_OpCode_OPC_SET_GATES, FollowerOpKind::SetGates},
      {wl_OpCode_OPC_BOOT_INFO, FollowerOpKind::BootInfo},
      {wl_OpCode_OPC_BOOT_DUMP, FollowerOpKind::BootDump},
      {wl_OpCode_OPC_BOOT_UPDATE, FollowerOpKind::BootUpdate},
      {wl_OpCode_OPC_UPDATE_UNITS, FollowerOpKind::ReflashUnit},
      {wl_OpCode_OPC_PROBE, FollowerOpKind::Probe},
  };
  // Every code the schema names is in the table: a new one must be planned.
  TEST_ASSERT_EQUAL(_wl_OpCode_MAX, sizeof(table) / sizeof(table[0]));
  for (const auto& row : table) {
    FollowerLinkOpPlan p = plan(row.code, 1);
    TEST_ASSERT_EQUAL_MESSAGE(wl_OpRefusal_REFUSAL_NONE, p.refusal, "refused");
    TEST_ASSERT_EQUAL((int)row.kind, (int)p.kind);
  }
}

static void test_a_code_this_build_does_not_know_is_refused() {
  TEST_ASSERT_EQUAL(wl_OpRefusal_REFUSAL_UNKNOWN_OP, plan(wl_OpCode_OPC_NONE, 1).refusal);
  FollowerLinkOpPlan p = plan(_wl_OpCode_MAX + 1, 1);
  TEST_ASSERT_EQUAL(wl_OpRefusal_REFUSAL_UNKNOWN_OP, p.refusal);
  TEST_ASSERT_EQUAL((int)FollowerOpKind::None, (int)p.kind);
}

static void test_a_unit_job_needs_a_running_unit_this_build_can_drive() {
  TEST_ASSERT_EQUAL(wl_OpRefusal_REFUSAL_NONE, plan(wl_OpCode_OPC_HOME, 1).refusal);
  TEST_ASSERT_EQUAL(wl_OpRefusal_REFUSAL_NO_UNIT, plan(wl_OpCode_OPC_HOME, 2).refusal);
  TEST_ASSERT_EQUAL(wl_OpRefusal_REFUSAL_UNIT_PROTOCOL, plan(wl_OpCode_OPC_HOME, 3).refusal);
  TEST_ASSERT_EQUAL(wl_OpRefusal_REFUSAL_NO_UNIT, plan(wl_OpCode_OPC_HOME, 4).refusal);
  TEST_ASSERT_EQUAL(wl_OpRefusal_REFUSAL_NO_UNIT,
                    plan(wl_OpCode_OPC_HOME, UNITS_AMOUNT + 1).refusal);
  TEST_ASSERT_EQUAL(wl_OpRefusal_REFUSAL_BAD_ADDRESS, plan(wl_OpCode_OPC_HOME, 0).refusal);
  TEST_ASSERT_EQUAL(wl_OpRefusal_REFUSAL_BAD_ADDRESS, plan(wl_OpCode_OPC_HOME, 127).refusal);
  TEST_ASSERT_EQUAL(wl_OpRefusal_REFUSAL_BAD_ADDRESS,
                    plan(wl_OpCode_OPC_HOME, 0xFFFFFFFFu).refusal);
}

static void test_arguments_take_the_shared_limits() {
  FollowerLinkOpPlan p = plan(wl_OpCode_OPC_JOG, 1, -127);
  TEST_ASSERT_EQUAL(wl_OpRefusal_REFUSAL_NONE, p.refusal);
  TEST_ASSERT_EQUAL(1, p.addr);
  TEST_ASSERT_EQUAL(-127, p.arg);
  TEST_ASSERT_EQUAL(wl_OpRefusal_REFUSAL_BAD_ARG, plan(wl_OpCode_OPC_JOG, 1, 128).refusal);
  TEST_ASSERT_EQUAL(wl_OpRefusal_REFUSAL_NONE,
                    plan(wl_OpCode_OPC_SET_OFFSET, 1, -SFP_OFFSET_LIMIT_STEPS).refusal);
  TEST_ASSERT_EQUAL(wl_OpRefusal_REFUSAL_BAD_ARG,
                    plan(wl_OpCode_OPC_SET_OFFSET, 1, SFP_OFFSET_LIMIT_STEPS + 1).refusal);
  TEST_ASSERT_EQUAL(wl_OpRefusal_REFUSAL_NONE,
                    plan(wl_OpCode_OPC_SET_GATES, 1, SFP_UNIT_GATE_IMPLEMENTED).refusal);
  TEST_ASSERT_EQUAL(wl_OpRefusal_REFUSAL_BAD_ARG, plan(wl_OpCode_OPC_SET_GATES, 1, 256).refusal);
  // The unit is checked before the argument: the nearer cause is named.
  TEST_ASSERT_EQUAL(wl_OpRefusal_REFUSAL_NO_UNIT, plan(wl_OpCode_OPC_JOG, 2, 999).refusal);
}

static void test_restarting_a_unit_needs_no_running_unit() {
  // The way back for a unit in its bootloader or on an unknown protocol.
  TEST_ASSERT_EQUAL(wl_OpRefusal_REFUSAL_NONE, plan(wl_OpCode_OPC_RESTART_UNIT, 2).refusal);
  TEST_ASSERT_EQUAL(wl_OpRefusal_REFUSAL_NONE, plan(wl_OpCode_OPC_RESTART_UNIT, 3).refusal);
  TEST_ASSERT_EQUAL(wl_OpRefusal_REFUSAL_NONE, plan(wl_OpCode_OPC_RESTART_UNIT, 126).refusal);
  TEST_ASSERT_EQUAL(wl_OpRefusal_REFUSAL_BAD_ADDRESS,
                    plan(wl_OpCode_OPC_RESTART_UNIT, 0).refusal);
  TEST_ASSERT_EQUAL(wl_OpRefusal_REFUSAL_BAD_ADDRESS,
                    plan(wl_OpCode_OPC_RESTART_UNIT, 127).refusal);
}

static void test_updating_units_is_the_whole_row_or_one_unit() {
  FollowerLinkOpPlan p = plan(wl_OpCode_OPC_UPDATE_UNITS, 0);
  TEST_ASSERT_EQUAL(wl_OpRefusal_REFUSAL_NONE, p.refusal);
  TEST_ASSERT_EQUAL(0, p.addr);
  // One unit, also when it sits in its bootloader, and forced when asked.
  p = plan(wl_OpCode_OPC_UPDATE_UNITS, 2, 1);
  TEST_ASSERT_EQUAL(wl_OpRefusal_REFUSAL_NONE, p.refusal);
  TEST_ASSERT_EQUAL(2, p.addr);
  TEST_ASSERT_EQUAL(1, p.arg);
  // Never every unit regardless of what it runs.
  TEST_ASSERT_EQUAL(wl_OpRefusal_REFUSAL_BAD_ARG, plan(wl_OpCode_OPC_UPDATE_UNITS, 0, 1).refusal);
  TEST_ASSERT_EQUAL(wl_OpRefusal_REFUSAL_BAD_ARG, plan(wl_OpCode_OPC_UPDATE_UNITS, 2, 2).refusal);
  TEST_ASSERT_EQUAL(wl_OpRefusal_REFUSAL_BAD_ADDRESS,
                    plan(wl_OpCode_OPC_UPDATE_UNITS, UNITS_AMOUNT + 1).refusal);
}

static void test_a_probe_ignores_the_address() {
  TEST_ASSERT_EQUAL(wl_OpRefusal_REFUSAL_NONE, plan(wl_OpCode_OPC_PROBE, 0).refusal);
  TEST_ASSERT_EQUAL(wl_OpRefusal_REFUSAL_NONE, plan(wl_OpCode_OPC_PROBE, 200).refusal);
}

static void test_how_a_job_ended_becomes_a_phase() {
  TEST_ASSERT_EQUAL(wl_OpPhase_OP_RUNNING, followerLinkPhaseFor(MaintOutcome::Pending));
  TEST_ASSERT_EQUAL(wl_OpPhase_OP_OK, followerLinkPhaseFor(MaintOutcome::Ok));
  TEST_ASSERT_EQUAL(wl_OpPhase_OP_FAILED, followerLinkPhaseFor(MaintOutcome::WireFail));
  TEST_ASSERT_EQUAL(wl_OpPhase_OP_FAILED, followerLinkPhaseFor(MaintOutcome::PostconditionFail));
  TEST_ASSERT_EQUAL(wl_OpPhase_OP_FAILED,
                    followerLinkPhaseFor(MaintOutcome::ExecValidationFail));
}

static void test_a_boot_section_travels_in_three_pieces() {
  const uint32_t pieceMax = sizeof(((wl_OpState*)0)->data.bytes);
  uint32_t offset = 0, pieces = 0;
  while (uint32_t n = followerLinkPieceLen(BOOT_SECTION_LEN, offset, pieceMax)) {
    offset += n;
    pieces++;
  }
  TEST_ASSERT_EQUAL(BOOT_SECTION_LEN, offset);
  TEST_ASSERT_EQUAL(3, pieces);
  TEST_ASSERT_EQUAL(0, followerLinkPieceLen(BOOT_SECTION_LEN, BOOT_SECTION_LEN + 5, pieceMax));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_every_job_code_has_its_op);
  RUN_TEST(test_a_code_this_build_does_not_know_is_refused);
  RUN_TEST(test_a_unit_job_needs_a_running_unit_this_build_can_drive);
  RUN_TEST(test_arguments_take_the_shared_limits);
  RUN_TEST(test_restarting_a_unit_needs_no_running_unit);
  RUN_TEST(test_updating_units_is_the_whole_row_or_one_unit);
  RUN_TEST(test_a_probe_ignores_the_address);
  RUN_TEST(test_how_a_job_ended_becomes_a_phase);
  RUN_TEST(test_a_boot_section_travels_in_three_pieces);
  return UNITY_END();
}
