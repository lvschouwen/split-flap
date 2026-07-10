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
  return UNITY_END();
}
