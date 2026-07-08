// Host-side unit tests for the pure protocol helpers in UnitProtocolHelpers.h
// (master<->unit I2C wire format logic, no Arduino deps).

#include <ArduinoFake.h>
#include <unity.h>
#include <string.h>
#include "../../UnitProtocolHelpers.h"
#include "SplitFlapProtocol.h"  // shared master<->unit contract (#149)

void setUp() {
  ArduinoFakeReset();
}
void tearDown() {}

// --- Shared protocol header self-consistency (#149) ---------------------
// Both firmwares #include SplitFlapProtocol.h, so the C side can't drift by
// construction. These guard the header itself: an accidental alphabet edit,
// a wrong flap count, or an opcode that collides with a letter index.

static void test_alphabet_matches_expected() {
  // The canonical 45-char drum alphabet. If this ever legitimately changes,
  // update the drums, script.js AND this expectation together.
  TEST_ASSERT_EQUAL_STRING(" ABCDEFGHIJKLMNOPQRSTUVWXYZ$&#0123456789:.-?!", SFP_ALPHABET);
}

static void test_flap_amount_derives_from_alphabet() {
  TEST_ASSERT_EQUAL_INT(45, SFP_FLAP_AMOUNT);
  TEST_ASSERT_EQUAL_INT((int)(sizeof(SFP_ALPHABET) - 1), SFP_FLAP_AMOUNT);
  // The native env's -D FLAP_AMOUNT scaffolding must agree with the header.
  TEST_ASSERT_EQUAL_INT(FLAP_AMOUNT, SFP_FLAP_AMOUNT);
}

static void test_opcodes_never_alias_letter_indices() {
  // Every command opcode must be >= the flap count so the unit can tell a
  // letter index (0..44) from a command on the wire.
  TEST_ASSERT_TRUE(SFP_CMD_ENTER_BOOTLOADER >= SFP_FLAP_AMOUNT);
  TEST_ASSERT_TRUE(SFP_CMD_GET_VERSION      >= SFP_FLAP_AMOUNT);
  TEST_ASSERT_TRUE(SFP_CMD_IDENTIFY         >= SFP_FLAP_AMOUNT);
  // The cross-generation recovery opcodes are fixed forever.
  TEST_ASSERT_EQUAL_HEX8(0x80, SFP_CMD_ENTER_BOOTLOADER);
  TEST_ASSERT_EQUAL_HEX8(0x81, SFP_CMD_GET_VERSION);
}

// CMD_GET_LETTER readback: value + bitwise complement, index must be a real
// flap position (0..FLAP_AMOUNT-1).

static void test_valid_pair_accepted() {
  TEST_ASSERT_TRUE(letterReadbackValid(0, (uint8_t)~0, FLAP_AMOUNT));
  TEST_ASSERT_TRUE(letterReadbackValid(7, (uint8_t)~7, FLAP_AMOUNT));
  TEST_ASSERT_TRUE(letterReadbackValid(FLAP_AMOUNT - 1, (uint8_t)~(FLAP_AMOUNT - 1), FLAP_AMOUNT));
}

static void test_wrong_complement_rejected() {
  TEST_ASSERT_FALSE(letterReadbackValid(7, (uint8_t)~8, FLAP_AMOUNT));
  TEST_ASSERT_FALSE(letterReadbackValid(7, 7, FLAP_AMOUNT));
  // Old-firmware fallback pattern: status byte 0/1 followed by bus garbage.
  TEST_ASSERT_FALSE(letterReadbackValid(0, 0, FLAP_AMOUNT));
  TEST_ASSERT_FALSE(letterReadbackValid(1, 0xFF, FLAP_AMOUNT));
}

static void test_out_of_range_index_rejected_even_with_valid_complement() {
  TEST_ASSERT_FALSE(letterReadbackValid(FLAP_AMOUNT, (uint8_t)~FLAP_AMOUNT, FLAP_AMOUNT));
  TEST_ASSERT_FALSE(letterReadbackValid(0xFF, 0x00, FLAP_AMOUNT));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_alphabet_matches_expected);
  RUN_TEST(test_flap_amount_derives_from_alphabet);
  RUN_TEST(test_opcodes_never_alias_letter_indices);
  RUN_TEST(test_valid_pair_accepted);
  RUN_TEST(test_wrong_complement_rejected);
  RUN_TEST(test_out_of_range_index_rejected_even_with_valid_complement);
  return UNITY_END();
}
