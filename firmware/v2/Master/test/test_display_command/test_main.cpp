// Host-side tests for the display command builders (#187) — the pure core of
// the network→display IPC contract. Commands are PODs (queue-copyable, no
// String members) and carry every parameter the display task needs: senders
// bake in speed/alignment so the display domain never reaches into shared
// settings across cores.

#include <ArduinoFake.h>
#include <unity.h>

#include "../../DisplayCommand.h"

void setUp() {}
void tearDown() {}

// --- ShowText builder -------------------------------------------------------

static void test_show_text_sets_opcode_and_copies_text() {
  DisplayCommand cmd = makeShowTextCommand("HELLO", "left", 50);
  TEST_ASSERT_EQUAL(DisplayOpcode::ShowText, cmd.opcode);
  TEST_ASSERT_EQUAL_STRING("HELLO", cmd.text);
}

static void test_show_text_is_nul_terminated_at_full_width() {
  // Exactly UNITS_AMOUNT characters must survive intact.
  String full;
  for (int i = 0; i < UNITS_AMOUNT; i++) full += 'A';
  DisplayCommand cmd = makeShowTextCommand(full, "left", 50);
  TEST_ASSERT_EQUAL(UNITS_AMOUNT, (int)strlen(cmd.text));
}

static void test_show_text_truncates_beyond_units_amount() {
  String over;
  for (int i = 0; i < UNITS_AMOUNT + 5; i++) over += 'B';
  DisplayCommand cmd = makeShowTextCommand(over, "left", 50);
  TEST_ASSERT_EQUAL(UNITS_AMOUNT, (int)strlen(cmd.text));
}

static void test_show_text_empty_text_is_valid() {
  DisplayCommand cmd = makeShowTextCommand("", "left", 50);
  TEST_ASSERT_EQUAL(DisplayOpcode::ShowText, cmd.opcode);
  TEST_ASSERT_EQUAL_STRING("", cmd.text);
}

// --- speed clamping (web-side 1..100 scale, v1 slider contract) --------------

static void test_speed_in_range_is_preserved() {
  TEST_ASSERT_EQUAL(80, makeShowTextCommand("X", "left", 80).speed);
}

static void test_speed_clamped_low() {
  TEST_ASSERT_EQUAL(1, makeShowTextCommand("X", "left", 0).speed);
  TEST_ASSERT_EQUAL(1, makeShowTextCommand("X", "left", -7).speed);
}

static void test_speed_clamped_high() {
  TEST_ASSERT_EQUAL(100, makeShowTextCommand("X", "left", 101).speed);
  TEST_ASSERT_EQUAL(100, makeShowTextCommand("X", "left", 100000).speed);
}

// --- alignment mapping -------------------------------------------------------

static void test_alignment_strings_map_to_enum() {
  TEST_ASSERT_EQUAL(DisplayAlignment::Left,
                    makeShowTextCommand("X", "left", 50).alignment);
  TEST_ASSERT_EQUAL(DisplayAlignment::Center,
                    makeShowTextCommand("X", "center", 50).alignment);
  TEST_ASSERT_EQUAL(DisplayAlignment::Right,
                    makeShowTextCommand("X", "right", 50).alignment);
}

static void test_unknown_alignment_falls_back_to_left() {
  TEST_ASSERT_EQUAL(DisplayAlignment::Left,
                    makeShowTextCommand("X", "diagonal", 50).alignment);
  TEST_ASSERT_EQUAL(DisplayAlignment::Left,
                    makeShowTextCommand("X", "", 50).alignment);
}

// --- Probe builder ------------------------------------------------------------

static void test_probe_command_carries_no_text() {
  DisplayCommand cmd = makeProbeCommand();
  TEST_ASSERT_EQUAL(DisplayOpcode::Probe, cmd.opcode);
  TEST_ASSERT_EQUAL_STRING("", cmd.text);
}

// --- describe (the stub worker's USB-CDC log line) ----------------------------

static void test_describe_show_text_names_opcode_and_text() {
  DisplayCommand cmd = makeShowTextCommand("HI", "center", 42);
  String line = describeDisplayCommand(cmd);
  TEST_ASSERT_TRUE(line.indexOf("ShowText") >= 0);
  TEST_ASSERT_TRUE(line.indexOf("HI") >= 0);
  TEST_ASSERT_TRUE(line.indexOf("42") >= 0);
  TEST_ASSERT_TRUE(line.indexOf("center") >= 0);
}

static void test_describe_probe_names_opcode() {
  String line = describeDisplayCommand(makeProbeCommand());
  TEST_ASSERT_TRUE(line.indexOf("Probe") >= 0);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_show_text_sets_opcode_and_copies_text);
  RUN_TEST(test_show_text_is_nul_terminated_at_full_width);
  RUN_TEST(test_show_text_truncates_beyond_units_amount);
  RUN_TEST(test_show_text_empty_text_is_valid);
  RUN_TEST(test_speed_in_range_is_preserved);
  RUN_TEST(test_speed_clamped_low);
  RUN_TEST(test_speed_clamped_high);
  RUN_TEST(test_alignment_strings_map_to_enum);
  RUN_TEST(test_unknown_alignment_falls_back_to_left);
  RUN_TEST(test_probe_command_carries_no_text);
  RUN_TEST(test_describe_show_text_names_opcode_and_text);
  RUN_TEST(test_describe_probe_names_opcode);
  return UNITY_END();
}
