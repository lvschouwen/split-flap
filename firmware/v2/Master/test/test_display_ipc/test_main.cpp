// Host-side tests for the display snapshot + worker transitions (#187, #203).
// The snapshot is the display task's published state: written only by the
// display task, copied out (mutex-guarded, in Tasks.cpp) by everyone else.
// displayApplyCommand() is the pure per-command state transition;
// displayApplyUnitFacts() folds the unit bus's probe/health facts into the
// snapshot and derives width + counts.

#include <ArduinoFake.h>
#include <unity.h>

#include "../../DisplayIpc.h"

void setUp() {}
void tearDown() {}

static void test_fresh_snapshot_defaults() {
  DisplaySnapshot snap;
  // v1 probe-fallback parity: nothing probed yet → hardware ceiling.
  TEST_ASSERT_EQUAL(UNITS_AMOUNT, snap.displayWidth);
  TEST_ASSERT_FALSE(snap.busy);
  TEST_ASSERT_EQUAL(0, snap.commandsProcessed);
  TEST_ASSERT_EQUAL_STRING("", snap.currentText);
}

static void test_show_text_updates_current_text_and_counter() {
  DisplaySnapshot snap;
  TEST_ASSERT_TRUE(displayApplyCommand(snap, makeShowTextCommand("HELLO", "left", 50)));
  TEST_ASSERT_EQUAL_STRING("HELLO", snap.currentText);
  TEST_ASSERT_EQUAL(1, snap.commandsProcessed);
}

static void test_second_show_text_replaces_text() {
  DisplaySnapshot snap;
  displayApplyCommand(snap, makeShowTextCommand("FIRST", "left", 50));
  displayApplyCommand(snap, makeShowTextCommand("SECOND", "left", 50));
  TEST_ASSERT_EQUAL_STRING("SECOND", snap.currentText);
  TEST_ASSERT_EQUAL(2, snap.commandsProcessed);
}

static void test_probe_counts_without_touching_width() {
  DisplaySnapshot snap;
  TEST_ASSERT_TRUE(displayApplyCommand(snap, makeProbeCommand()));
  // The command transition only counts; width/facts come from the bus scan
  // via displayApplyUnitFacts() (the glue calls both).
  TEST_ASSERT_EQUAL(UNITS_AMOUNT, snap.displayWidth);
  TEST_ASSERT_EQUAL(1, snap.commandsProcessed);
}

static void test_unit_facts_derive_width_and_counts() {
  DisplaySnapshot snap;
  UnitFacts facts[UNITS_AMOUNT];
  facts[0].state = 1;
  facts[2].state = 2;  // bootloader counts as present (#123 width rule)
  displayApplyUnitFacts(snap, facts, UNITS_AMOUNT);
  // Highest responder + 1; the silent gap at index 1 keeps its slot.
  TEST_ASSERT_EQUAL(3, snap.displayWidth);
  TEST_ASSERT_EQUAL(2, snap.detectedUnitCount);
  TEST_ASSERT_EQUAL(1, snap.units[0].state);
  TEST_ASSERT_EQUAL(0, snap.units[1].state);
  TEST_ASSERT_EQUAL(2, snap.units[2].state);
}

static void test_unit_facts_all_silent_falls_back_to_ceiling() {
  DisplaySnapshot snap;
  UnitFacts facts[UNITS_AMOUNT];  // defaults: all silent
  displayApplyUnitFacts(snap, facts, UNITS_AMOUNT);
  // v1 parity: a unit-less bench master still lays out full-width.
  TEST_ASSERT_EQUAL(UNITS_AMOUNT, snap.displayWidth);
  TEST_ASSERT_EQUAL(0, snap.detectedUnitCount);
}

static void test_unit_facts_recompute_faulty_count() {
  DisplaySnapshot snap;
  UnitFacts facts[UNITS_AMOUNT];
  facts[0].state = 1;
  facts[0].statusValid = true;
  facts[0].status.lifetimeBrownoutCount = 1;  // faulty
  facts[1].state = 1;
  facts[1].statusValid = true;                // clean
  displayApplyUnitFacts(snap, facts, UNITS_AMOUNT);
  TEST_ASSERT_EQUAL(1, snap.faultyUnitCount);
  // A later all-clean pass must clear the count, not latch it.
  facts[0].status.lifetimeBrownoutCount = 0;
  displayApplyUnitFacts(snap, facts, UNITS_AMOUNT);
  TEST_ASSERT_EQUAL(0, snap.faultyUnitCount);
}

static void test_none_opcode_is_rejected_without_mutation() {
  DisplaySnapshot snap;
  displayApplyCommand(snap, makeShowTextCommand("KEEP", "left", 50));
  DisplayCommand none;  // default-constructed: opcode None
  TEST_ASSERT_FALSE(displayApplyCommand(snap, none));
  TEST_ASSERT_EQUAL_STRING("KEEP", snap.currentText);
  TEST_ASSERT_EQUAL(1, snap.commandsProcessed);
}

// --- maintenance transitions (#204) ---------------------------------------------

static void test_maintenance_opcodes_count_and_apply() {
  DisplaySnapshot snap;
  TEST_ASSERT_TRUE(displayApplyCommand(snap, makeWriteOffsetCommand(1, 3, 10)));
  TEST_ASSERT_TRUE(displayApplyCommand(snap, makeJogCommand(2, 3, -5)));
  TEST_ASSERT_TRUE(displayApplyCommand(snap, makeHomeCommand(3, 3)));
  TEST_ASSERT_TRUE(displayApplyCommand(snap, makeIdentifyCommand(4, 3)));
  TEST_ASSERT_TRUE(displayApplyCommand(snap, makeRebootToBootloaderCommand(5, 3)));
  TEST_ASSERT_TRUE(displayApplyCommand(snap, makeSetAddressCommand(6, 3, 4)));
  TEST_ASSERT_TRUE(displayApplyCommand(snap, makeClearAddressCommand(7, 3)));
  // #231 review CRITICAL: displayApplyCommand gates the whole dispatch in
  // Tasks.cpp — an opcode missing here silently never executes on the bus.
  TEST_ASSERT_TRUE(displayApplyCommand(snap, makeResetOdometerCommand(8, 3)));
  TEST_ASSERT_EQUAL(8, snap.commandsProcessed);
}

static void test_stop_clears_current_text() {
  DisplaySnapshot snap;
  displayApplyCommand(snap, makeShowTextCommand("HELLO", "left", 50));
  TEST_ASSERT_TRUE(displayApplyCommand(snap, makeStopCommand(8)));
  // v1 parity: the retained text clears so the clock/event loop re-sends
  // fresh content instead of dedup-suppressing forever.
  TEST_ASSERT_EQUAL_STRING("", snap.currentText);
}

static void test_reset_units_leaves_current_text_untouched() {
  DisplaySnapshot snap;
  displayApplyCommand(snap, makeShowTextCommand("HELLO", "left", 50));
  TEST_ASSERT_TRUE(
      displayApplyCommand(snap, makeResetUnitsCommand(9, "HELLO", "left", 50)));
  // The blank-out frames are execution detail; the re-shown text equals the
  // baked (enqueue-time) text, so the snapshot text stays truthful as-is.
  TEST_ASSERT_EQUAL_STRING("HELLO", snap.currentText);
}

// --- maintenance result slot (the /unit/op-result contract) ----------------------

static void test_fresh_snapshot_has_no_maint_result() {
  DisplaySnapshot snap;
  TEST_ASSERT_EQUAL_UINT32(0, snap.lastMaint.seq);
  TEST_ASSERT_EQUAL(MaintOutcome::Pending, snap.lastMaint.outcome);
}

static void test_apply_maint_result_fills_the_slot() {
  DisplaySnapshot snap;
  DisplayCommand cmd = makeWriteOffsetCommand(42, 3, 100);
  displayApplyMaintResult(snap, cmd, MaintOutcome::WireFail, MaintReason::None);
  TEST_ASSERT_EQUAL_UINT32(42, snap.lastMaint.seq);
  TEST_ASSERT_EQUAL(DisplayOpcode::WriteOffset, snap.lastMaint.opcode);
  TEST_ASSERT_EQUAL_UINT8(3, snap.lastMaint.addr);
  TEST_ASSERT_EQUAL(MaintOutcome::WireFail, snap.lastMaint.outcome);
}

static void test_op_result_query_states() {
  DisplaySnapshot snap;
  // Nothing executed yet: any queried seq is still pending.
  TEST_ASSERT_EQUAL(OpResultState::Pending, opResultQuery(snap.lastMaint, 5));
  DisplayCommand cmd = makeHomeCommand(5, 3);
  displayApplyMaintResult(snap, cmd, MaintOutcome::Ok, MaintReason::None);
  TEST_ASSERT_EQUAL(OpResultState::Found, opResultQuery(snap.lastMaint, 5));
  // The slot advanced past an older seq: its outcome is gone for good.
  TEST_ASSERT_EQUAL(OpResultState::Expired, opResultQuery(snap.lastMaint, 4));
  TEST_ASSERT_EQUAL(OpResultState::Pending, opResultQuery(snap.lastMaint, 6));
}

static void test_op_result_json_shapes() {
  DisplaySnapshot snap;
  char buf[96];
  buildOpResultJson(buf, sizeof(buf), snap.lastMaint, 5);
  TEST_ASSERT_EQUAL_STRING("{\"state\":\"pending\"}", buf);
  displayApplyMaintResult(snap, makeHomeCommand(5, 3), MaintOutcome::Ok,
                          MaintReason::None);
  buildOpResultJson(buf, sizeof(buf), snap.lastMaint, 5);
  TEST_ASSERT_EQUAL_STRING("{\"state\":\"ok\"}", buf);
  buildOpResultJson(buf, sizeof(buf), snap.lastMaint, 4);
  TEST_ASSERT_EQUAL_STRING("{\"state\":\"expired\"}", buf);
  displayApplyMaintResult(snap, makeSetAddressCommand(6, 3, 7),
                          MaintOutcome::PostconditionFail,
                          MaintReason::UnitMissingAfterReprobe);
  buildOpResultJson(buf, sizeof(buf), snap.lastMaint, 6);
  TEST_ASSERT_EQUAL_STRING(
      "{\"state\":\"failed\",\"reason\":\"postcondition-fail\","
      "\"detail\":\"unit-missing-after-reprobe\"}",
      buf);
  displayApplyMaintResult(snap, makeJogCommand(7, 3, 5), MaintOutcome::WireFail,
                          MaintReason::None);
  buildOpResultJson(buf, sizeof(buf), snap.lastMaint, 7);
  TEST_ASSERT_EQUAL_STRING("{\"state\":\"failed\",\"reason\":\"wire-fail\"}",
                           buf);
  // Pre-burn exec recheck refused the target (stale web-side view).
  displayApplyMaintResult(snap, makeSetAddressCommand(8, 3, 5),
                          MaintOutcome::ExecValidationFail,
                          MaintReason::TargetAddressOccupied);
  buildOpResultJson(buf, sizeof(buf), snap.lastMaint, 8);
  TEST_ASSERT_EQUAL_STRING(
      "{\"state\":\"failed\",\"reason\":\"exec-validation-fail\","
      "\"detail\":\"target-address-occupied\"}",
      buf);
}

// --- offset facts (#204: probe-time truth, patched on successful write) ----------

static void test_offset_fact_defaults_invalid() {
  DisplaySnapshot snap;
  TEST_ASSERT_FALSE(snap.units[0].offsetValid);
}

static void test_apply_offset_write_patches_the_fact_in_place() {
  DisplaySnapshot snap;
  UnitFacts facts[UNITS_AMOUNT];
  facts[2].state = 1;
  displayApplyUnitFacts(snap, facts, UNITS_AMOUNT);
  displayApplyOffsetWrite(snap, 3, -450);
  TEST_ASSERT_TRUE(snap.units[2].offsetValid);
  TEST_ASSERT_EQUAL_INT16(-450, snap.units[2].offset);
}

static void test_invalidate_unit_reads_after_bootloader_reboot() {
  DisplaySnapshot snap;
  UnitFacts facts[UNITS_AMOUNT];
  facts[2].state = 1;
  facts[2].statusValid = true;
  facts[2].offset = 10;
  facts[2].offsetValid = true;
  displayApplyUnitFacts(snap, facts, UNITS_AMOUNT);
  displayInvalidateUnitReads(snap, 3);
  TEST_ASSERT_FALSE(snap.units[2].offsetValid);
  TEST_ASSERT_FALSE(snap.units[2].statusValid);
}

// --- reflash job (#205): progress in the snapshot + the producer gate ---------

static void test_fresh_snapshot_reflash_is_idle_and_accepts_commands() {
  DisplaySnapshot snap;
  TEST_ASSERT_EQUAL(ReflashState::Idle, snap.reflash.state);
  TEST_ASSERT_TRUE(displayAcceptsCommand(snap, DisplayOpcode::ShowText));
  TEST_ASSERT_TRUE(displayAcceptsCommand(snap, DisplayOpcode::Jog));
  TEST_ASSERT_TRUE(displayAcceptsCommand(snap, DisplayOpcode::ReflashUnits));
}

static void test_gate_blocks_everything_but_stop_while_reflashing() {
  DisplaySnapshot snap;
  reflashProgressBegin(snap.reflash, 4);
  TEST_ASSERT_FALSE(displayAcceptsCommand(snap, DisplayOpcode::ShowText));
  TEST_ASSERT_FALSE(displayAcceptsCommand(snap, DisplayOpcode::Probe));
  TEST_ASSERT_FALSE(displayAcceptsCommand(snap, DisplayOpcode::Jog));
  TEST_ASSERT_FALSE(displayAcceptsCommand(snap, DisplayOpcode::ResetUnits));
  TEST_ASSERT_FALSE(displayAcceptsCommand(snap, DisplayOpcode::ReflashUnits));
  TEST_ASSERT_TRUE(displayAcceptsCommand(snap, DisplayOpcode::Stop));
}

static void test_gate_reopens_after_job_finishes() {
  DisplaySnapshot snap;
  reflashProgressBegin(snap.reflash, 4);
  reflashProgressFinish(snap.reflash, false);
  TEST_ASSERT_TRUE(displayAcceptsCommand(snap, DisplayOpcode::ShowText));
}

static void test_reflash_units_command_counts_without_touching_text() {
  DisplaySnapshot snap;
  displayApplyCommand(snap, makeShowTextCommand("14:44", "center", 80));
  DisplayCommand cmd = makeReflashUnitsCommand(7, "14:44", "center", 80);
  TEST_ASSERT_TRUE(displayApplyCommand(snap, cmd));
  TEST_ASSERT_EQUAL_STRING("14:44", snap.currentText);
  TEST_ASSERT_EQUAL(2, snap.commandsProcessed);
}

static void test_reflash_json_shapes() {
  char buf[96];
  ReflashProgress p;
  buildReflashJson(buf, sizeof(buf), p);
  TEST_ASSERT_EQUAL_STRING(
      "{\"state\":\"idle\",\"total\":0,\"done\":0,\"failed\":0,\"cur\":0}",
      buf);
  reflashProgressBegin(p, 12);
  reflashProgressUnitStart(p, 5);
  reflashProgressUnitResult(p, true);
  reflashProgressUnitStart(p, 6);
  buildReflashJson(buf, sizeof(buf), p);
  TEST_ASSERT_EQUAL_STRING(
      "{\"state\":\"flashing\",\"total\":12,\"done\":1,\"failed\":0,\"cur\":6}",
      buf);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_fresh_snapshot_defaults);
  RUN_TEST(test_show_text_updates_current_text_and_counter);
  RUN_TEST(test_second_show_text_replaces_text);
  RUN_TEST(test_probe_counts_without_touching_width);
  RUN_TEST(test_unit_facts_derive_width_and_counts);
  RUN_TEST(test_unit_facts_all_silent_falls_back_to_ceiling);
  RUN_TEST(test_unit_facts_recompute_faulty_count);
  RUN_TEST(test_none_opcode_is_rejected_without_mutation);
  RUN_TEST(test_maintenance_opcodes_count_and_apply);
  RUN_TEST(test_stop_clears_current_text);
  RUN_TEST(test_reset_units_leaves_current_text_untouched);
  RUN_TEST(test_fresh_snapshot_has_no_maint_result);
  RUN_TEST(test_apply_maint_result_fills_the_slot);
  RUN_TEST(test_op_result_query_states);
  RUN_TEST(test_op_result_json_shapes);
  RUN_TEST(test_offset_fact_defaults_invalid);
  RUN_TEST(test_apply_offset_write_patches_the_fact_in_place);
  RUN_TEST(test_invalidate_unit_reads_after_bootloader_reboot);
  RUN_TEST(test_fresh_snapshot_reflash_is_idle_and_accepts_commands);
  RUN_TEST(test_gate_blocks_everything_but_stop_while_reflashing);
  RUN_TEST(test_gate_reopens_after_job_finishes);
  RUN_TEST(test_reflash_units_command_counts_without_touching_text);
  RUN_TEST(test_reflash_json_shapes);
  return UNITY_END();
}
