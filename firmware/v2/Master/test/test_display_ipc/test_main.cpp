// Host-side tests for the display snapshot + stub-worker transition (#187).
// The snapshot is the display task's published state: written only by the
// display task, copied out (mutex-guarded, in Tasks.cpp) by everyone else.
// displayApplyCommand() is the stub worker's pure state transition — the
// real I2C slice replaces its internals, not its contract.

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

static void test_probe_counts_but_keeps_stub_width() {
  DisplaySnapshot snap;
  TEST_ASSERT_TRUE(displayApplyCommand(snap, makeProbeCommand()));
  // Stub worker: no bus yet, width stays at the fallback ceiling.
  TEST_ASSERT_EQUAL(UNITS_AMOUNT, snap.displayWidth);
  TEST_ASSERT_EQUAL(1, snap.commandsProcessed);
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
  RUN_TEST(test_probe_counts_but_keeps_stub_width);
  RUN_TEST(test_none_opcode_is_rejected_without_mutation);
  return UNITY_END();
}
