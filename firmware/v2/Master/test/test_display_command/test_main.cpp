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

// --- truncateForDisplay (#192 review H1) --------------------------------------

static void test_truncate_for_display_caps_at_units_amount() {
  String over;
  for (int i = 0; i < UNITS_AMOUNT + 5; i++) over += 'C';
  TEST_ASSERT_EQUAL(UNITS_AMOUNT, (int)truncateForDisplay(over).length());
}

static void test_truncate_for_display_passes_short_text_unchanged() {
  TEST_ASSERT_EQUAL_STRING("HELLO", truncateForDisplay("HELLO").c_str());
  TEST_ASSERT_EQUAL_STRING("", truncateForDisplay("").c_str());
}

static void test_truncate_matches_command_builder_domain() {
  // ClockPolicy's dedup comparisons only work if this helper yields EXACTLY
  // what makeShowTextCommand writes into the queue slot.
  String over;
  for (int i = 0; i < UNITS_AMOUNT + 5; i++) over += (char)('A' + (i % 26));
  DisplayCommand cmd = makeShowTextCommand(over, "left", 50);
  TEST_ASSERT_EQUAL_STRING(cmd.text, truncateForDisplay(over).c_str());
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

// --- maintenance builders (#204) -----------------------------------------------
// Every op is a POD command built ONLY through these makers: seq is the
// op-result correlation handle, unitAddress the wire target, value the
// opcode-disambiguated payload (offset steps / jog steps / new address).

static void test_write_offset_command_bakes_seq_addr_and_negative_value() {
  DisplayCommand cmd = makeWriteOffsetCommand(7, 3, -1200);
  TEST_ASSERT_EQUAL(DisplayOpcode::WriteOffset, cmd.opcode);
  TEST_ASSERT_EQUAL_UINT32(7, cmd.seq);
  TEST_ASSERT_EQUAL_UINT8(3, cmd.unitAddress);
  TEST_ASSERT_EQUAL_INT16(-1200, cmd.value);
}

static void test_jog_command_clamps_to_int8_range() {
  TEST_ASSERT_EQUAL_INT16(127, makeJogCommand(1, 2, 400).value);
  TEST_ASSERT_EQUAL_INT16(-127, makeJogCommand(1, 2, -400).value);
  TEST_ASSERT_EQUAL_INT16(-5, makeJogCommand(1, 2, -5).value);
  TEST_ASSERT_EQUAL(DisplayOpcode::Jog, makeJogCommand(1, 2, 1).opcode);
}

static void test_home_and_identify_and_reboot_carry_address_only() {
  TEST_ASSERT_EQUAL(DisplayOpcode::Home, makeHomeCommand(2, 5).opcode);
  TEST_ASSERT_EQUAL_UINT8(5, makeHomeCommand(2, 5).unitAddress);
  TEST_ASSERT_EQUAL(DisplayOpcode::Identify, makeIdentifyCommand(3, 6).opcode);
  TEST_ASSERT_EQUAL(DisplayOpcode::RebootToBootloader,
                    makeRebootToBootloaderCommand(4, 7).opcode);
  TEST_ASSERT_EQUAL_UINT8(7, makeRebootToBootloaderCommand(4, 7).unitAddress);
}

static void test_set_address_command_carries_target_in_value() {
  DisplayCommand cmd = makeSetAddressCommand(9, 4, 12);
  TEST_ASSERT_EQUAL(DisplayOpcode::SetAddress, cmd.opcode);
  TEST_ASSERT_EQUAL_UINT8(4, cmd.unitAddress);
  TEST_ASSERT_EQUAL_INT16(12, cmd.value);
}

static void test_clear_address_command() {
  DisplayCommand cmd = makeClearAddressCommand(10, 8);
  TEST_ASSERT_EQUAL(DisplayOpcode::ClearAddress, cmd.opcode);
  TEST_ASSERT_EQUAL_UINT8(8, cmd.unitAddress);
}

static void test_reset_units_bakes_enqueue_time_text_and_show_params() {
  // Senders bake params: the re-show text/alignment/speed are frozen at
  // enqueue, so a ShowText queued between ResetUnits and its execution
  // can't leak in and the re-show honors the settings of that moment.
  DisplayCommand cmd = makeResetUnitsCommand(11, "HELLO", "center", 42);
  TEST_ASSERT_EQUAL(DisplayOpcode::ResetUnits, cmd.opcode);
  TEST_ASSERT_EQUAL_UINT32(11, cmd.seq);
  TEST_ASSERT_EQUAL_STRING("HELLO", cmd.text);
  TEST_ASSERT_EQUAL(DisplayAlignment::Center, cmd.alignment);
  TEST_ASSERT_EQUAL_UINT8(42, cmd.speed);
}

static void test_reflash_units_bakes_enqueue_time_show_params() {
  // Same bake-at-enqueue rule as ResetUnits (#205): the job's end-of-run
  // re-show uses the content of the moment the operator clicked.
  DisplayCommand cmd = makeReflashUnitsCommand(21, "14:44", "center", 80);
  TEST_ASSERT_EQUAL(DisplayOpcode::ReflashUnits, cmd.opcode);
  TEST_ASSERT_EQUAL_UINT32(21, cmd.seq);
  TEST_ASSERT_EQUAL_STRING("14:44", cmd.text);
  TEST_ASSERT_EQUAL(DisplayAlignment::Center, cmd.alignment);
  TEST_ASSERT_EQUAL_UINT8(80, cmd.speed);
}

static void test_reflash_units_opcode_name() {
  TEST_ASSERT_EQUAL_STRING("ReflashUnits",
                           displayOpcodeName(DisplayOpcode::ReflashUnits));
}

static void test_reset_units_truncates_overlong_text() {
  DisplayCommand cmd =
      makeResetUnitsCommand(12, "0123456789ABCDEFOVERFLOW", "left", 50);
  TEST_ASSERT_EQUAL(DISPLAY_CMD_TEXT_LEN, (int)strlen(cmd.text));
}

static void test_stop_command_carries_seq_only() {
  DisplayCommand cmd = makeStopCommand(13);
  TEST_ASSERT_EQUAL(DisplayOpcode::Stop, cmd.opcode);
  TEST_ASSERT_EQUAL_UINT32(13, cmd.seq);
  TEST_ASSERT_EQUAL_UINT8(0, cmd.unitAddress);
}

static void test_describe_names_maintenance_opcodes() {
  TEST_ASSERT_TRUE(
      describeDisplayCommand(makeWriteOffsetCommand(1, 3, -10)).indexOf("WriteOffset") >= 0);
  TEST_ASSERT_TRUE(
      describeDisplayCommand(makeSetAddressCommand(1, 3, 4)).indexOf("SetAddress") >= 0);
  TEST_ASSERT_TRUE(
      describeDisplayCommand(makeStopCommand(1)).indexOf("Stop") >= 0);
}

// --- self-test op (#265) ---------------------------------------------------

static void test_selftest_command_carries_seq_and_address() {
  DisplayCommand cmd = makeSelfTestCommand(11, 4);
  TEST_ASSERT_TRUE(cmd.opcode == DisplayOpcode::SelfTest);
  TEST_ASSERT_EQUAL_UINT32(11, cmd.seq);
  TEST_ASSERT_EQUAL_UINT8(4, cmd.unitAddress);
  TEST_ASSERT_EQUAL_INT16(0, cmd.value);
}

static void test_set_gates_command_carries_the_gate_byte_in_value() {
  DisplayCommand cmd = makeSetGatesCommand(12, 5, 0x01);
  TEST_ASSERT_TRUE(cmd.opcode == DisplayOpcode::SetGates);
  TEST_ASSERT_EQUAL_UINT32(12, cmd.seq);
  TEST_ASSERT_EQUAL_UINT8(5, cmd.unitAddress);
  TEST_ASSERT_EQUAL_INT16(0x01, cmd.value);
}

// Clearing every gate is the safety action — it must survive the trip
// through the signed value field as 0, not as a dropped command.
static void test_set_gates_command_carries_an_all_clear() {
  DisplayCommand cmd = makeSetGatesCommand(13, 6, 0x00);
  TEST_ASSERT_TRUE(cmd.opcode == DisplayOpcode::SetGates);
  TEST_ASSERT_EQUAL_INT16(0, cmd.value);
}

static void test_set_gates_high_bit_survives_the_signed_value_field() {
  DisplayCommand cmd = makeSetGatesCommand(14, 7, 0xFF);
  TEST_ASSERT_EQUAL_UINT8(0xFF, (uint8_t)cmd.value);
}

static void test_set_gates_opcode_name() {
  TEST_ASSERT_EQUAL_STRING("SetGates", displayOpcodeName(DisplayOpcode::SetGates));
}

static void test_selftest_opcode_name() {
  TEST_ASSERT_EQUAL_STRING("SelfTest", displayOpcodeName(DisplayOpcode::SelfTest));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_show_text_sets_opcode_and_copies_text);
  RUN_TEST(test_show_text_is_nul_terminated_at_full_width);
  RUN_TEST(test_show_text_truncates_beyond_units_amount);
  RUN_TEST(test_show_text_empty_text_is_valid);
  RUN_TEST(test_truncate_for_display_caps_at_units_amount);
  RUN_TEST(test_truncate_for_display_passes_short_text_unchanged);
  RUN_TEST(test_truncate_matches_command_builder_domain);
  RUN_TEST(test_speed_in_range_is_preserved);
  RUN_TEST(test_speed_clamped_low);
  RUN_TEST(test_speed_clamped_high);
  RUN_TEST(test_alignment_strings_map_to_enum);
  RUN_TEST(test_unknown_alignment_falls_back_to_left);
  RUN_TEST(test_probe_command_carries_no_text);
  RUN_TEST(test_describe_show_text_names_opcode_and_text);
  RUN_TEST(test_describe_probe_names_opcode);
  RUN_TEST(test_write_offset_command_bakes_seq_addr_and_negative_value);
  RUN_TEST(test_jog_command_clamps_to_int8_range);
  RUN_TEST(test_home_and_identify_and_reboot_carry_address_only);
  RUN_TEST(test_set_address_command_carries_target_in_value);
  RUN_TEST(test_clear_address_command);
  RUN_TEST(test_reset_units_bakes_enqueue_time_text_and_show_params);
  RUN_TEST(test_reset_units_truncates_overlong_text);
  RUN_TEST(test_stop_command_carries_seq_only);
  RUN_TEST(test_describe_names_maintenance_opcodes);
  RUN_TEST(test_reflash_units_bakes_enqueue_time_show_params);
  RUN_TEST(test_reflash_units_opcode_name);
  RUN_TEST(test_selftest_command_carries_seq_and_address);
  RUN_TEST(test_selftest_opcode_name);
  RUN_TEST(test_set_gates_command_carries_the_gate_byte_in_value);
  RUN_TEST(test_set_gates_command_carries_an_all_clear);
  RUN_TEST(test_set_gates_high_bit_survives_the_signed_value_field);
  RUN_TEST(test_set_gates_opcode_name);
  return UNITY_END();
}
