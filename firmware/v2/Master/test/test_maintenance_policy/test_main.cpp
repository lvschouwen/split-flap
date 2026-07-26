// Host-side tests for the maintenance validation + classification policy
// (#204). This is the pure core behind the calibration/provisioning web
// endpoints AND displayTask's execution-time rechecks: the same occupancy
// check runs at the web boundary (fast 409) and right before the EEPROM
// burn (authoritative). Wire-byte encoders live here too so the negative
// int16/int8 encodings UnitBus puts on the bus are asserted natively.

#include <unity.h>

#include "../../MaintenancePolicy.h"

void setUp() {}
void tearDown() {}

// One sketch unit at address 3, one bootloader unit at 5, rest silent.
static void fillFacts(UnitFacts* facts) {
  for (int i = 0; i < UNITS_AMOUNT; i++) facts[i] = UnitFacts{};
  facts[2].state = 1;  // address 3
  facts[4].state = 2;  // address 5, twiboot
}

// --- address validation (v1 parseCalibrationAddress parity) -------------------

static void test_missing_address_is_400() {
  UnitFacts facts[UNITS_AMOUNT];
  fillFacts(facts);
  int addr = 0;
  MaintVerdict v = maintValidateAddress(nullptr, facts, UNITS_AMOUNT, addr);
  TEST_ASSERT_EQUAL(400, v.httpStatus);
}

static void test_unparsable_address_is_400() {
  UnitFacts facts[UNITS_AMOUNT];
  fillFacts(facts);
  int addr = 0;
  MaintVerdict v = maintValidateAddress("bogus", facts, UNITS_AMOUNT, addr);
  TEST_ASSERT_EQUAL(400, v.httpStatus);
}

static void test_address_out_of_i2c_range_is_400() {
  UnitFacts facts[UNITS_AMOUNT];
  fillFacts(facts);
  int addr = 0;
  TEST_ASSERT_EQUAL(400, maintValidateAddress("0", facts, UNITS_AMOUNT, addr).httpStatus);
  TEST_ASSERT_EQUAL(400, maintValidateAddress("127", facts, UNITS_AMOUNT, addr).httpStatus);
}

static void test_address_beyond_managed_units_is_404() {
  UnitFacts facts[UNITS_AMOUNT];
  fillFacts(facts);
  int addr = 0;
  // 17 is a legal I2C address but beyond UNITS_AMOUNT — no unit can be there.
  TEST_ASSERT_EQUAL(404, maintValidateAddress("17", facts, UNITS_AMOUNT, addr).httpStatus);
}

static void test_protocol_mismatch_unit_is_409_not_drivable() {
  // #405 gap found in review: this gate only checked state==1, so every
  // single-unit op — including SET_I2C_ADDRESS, an unverifiable address burn
  // whose recovery is a physical trip — would have been sent to a unit whose
  // reply layout we cannot parse.
  UnitFacts facts[UNITS_AMOUNT];
  fillFacts(facts);
  int addr = 0;
  facts[1].state = 1;  // present and sketch-running...
  facts[1].protocolKnown = true;
  facts[1].protocolVersion = SFP_PROTOCOL_VERSION;
  TEST_ASSERT_EQUAL(200, maintValidateAddress("2", facts, UNITS_AMOUNT, addr).httpStatus);
  TEST_ASSERT_EQUAL(2, addr);
  // ...but speaking a contract we do not have code for.
  facts[1].protocolVersion = (uint8_t)(SFP_PROTOCOL_VERSION + 1);
  MaintVerdict v = maintValidateAddress("2", facts, UNITS_AMOUNT, addr);
  TEST_ASSERT_EQUAL(409, v.httpStatus);  // present, but refused — not a 404
}

static void test_unread_protocol_stays_drivable() {
  // An UNREADABLE version is absence of evidence, not evidence of difference.
  // Refusing those would strand every unit whose version read simply failed.
  UnitFacts facts[UNITS_AMOUNT];
  fillFacts(facts);
  int addr = 0;
  facts[1].state = 1;
  facts[1].protocolKnown = false;
  facts[1].protocolVersion = 0;
  TEST_ASSERT_EQUAL(200, maintValidateAddress("2", facts, UNITS_AMOUNT, addr).httpStatus);
}

static void test_silent_and_bootloader_units_are_404() {
  UnitFacts facts[UNITS_AMOUNT];
  fillFacts(facts);
  int addr = 0;
  TEST_ASSERT_EQUAL(404, maintValidateAddress("1", facts, UNITS_AMOUNT, addr).httpStatus);
  TEST_ASSERT_EQUAL(404, maintValidateAddress("5", facts, UNITS_AMOUNT, addr).httpStatus);
}

static void test_sketch_unit_passes_and_yields_address() {
  UnitFacts facts[UNITS_AMOUNT];
  fillFacts(facts);
  int addr = 0;
  MaintVerdict v = maintValidateAddress("3", facts, UNITS_AMOUNT, addr);
  TEST_ASSERT_EQUAL(200, v.httpStatus);
  TEST_ASSERT_EQUAL(3, addr);
}

static void test_hex_address_parses_v1_strtol_base0() {
  UnitFacts facts[UNITS_AMOUNT];
  fillFacts(facts);
  int addr = 0;
  MaintVerdict v = maintValidateAddress("0x03", facts, UNITS_AMOUNT, addr);
  TEST_ASSERT_EQUAL(200, v.httpStatus);
  TEST_ASSERT_EQUAL(3, addr);
}

// --- payload validation --------------------------------------------------------

static void test_offset_limit_is_one_revolution_both_signs() {
  TEST_ASSERT_EQUAL(200, maintValidateOffset(SFP_OFFSET_LIMIT_STEPS).httpStatus);
  TEST_ASSERT_EQUAL(200, maintValidateOffset(-SFP_OFFSET_LIMIT_STEPS).httpStatus);
  TEST_ASSERT_EQUAL(400, maintValidateOffset(SFP_OFFSET_LIMIT_STEPS + 1).httpStatus);
  TEST_ASSERT_EQUAL(400, maintValidateOffset(-SFP_OFFSET_LIMIT_STEPS - 1).httpStatus);
}

static void test_jog_limit_is_int8_range() {
  TEST_ASSERT_EQUAL(200, maintValidateJog(127).httpStatus);
  TEST_ASSERT_EQUAL(200, maintValidateJog(-127).httpStatus);
  TEST_ASSERT_EQUAL(400, maintValidateJog(128).httpStatus);
  TEST_ASSERT_EQUAL(400, maintValidateJog(-128).httpStatus);
}

// Feature gates are one wire byte (#409). The web boundary bounds the byte
// and nothing else: WHICH bits are legal belongs to the unit's firmware,
// which refuses the ones it has no code for, and the write's read-back grades
// that refusal. A master enforcing a vocabulary here would reject gates a
// newer unit firmware understands.
static void test_gates_accept_any_byte_and_reject_beyond_it() {
  TEST_ASSERT_EQUAL(200, maintValidateGates(0).httpStatus);
  TEST_ASSERT_EQUAL(200, maintValidateGates(1).httpStatus);
  TEST_ASSERT_EQUAL(200, maintValidateGates(255).httpStatus);
  TEST_ASSERT_EQUAL(400, maintValidateGates(256).httpStatus);
  TEST_ASSERT_EQUAL(400, maintValidateGates(-1).httpStatus);
}

// --- set-address target (web boundary AND displayTask recheck) -----------------

static void test_set_address_target_out_of_managed_range_is_400() {
  UnitFacts facts[UNITS_AMOUNT];
  fillFacts(facts);
  TEST_ASSERT_EQUAL(400, maintValidateSetAddressTarget(0, 3, facts, UNITS_AMOUNT).httpStatus);
  TEST_ASSERT_EQUAL(400, maintValidateSetAddressTarget(UNITS_AMOUNT + 1, 3, facts, UNITS_AMOUNT).httpStatus);
}

static void test_set_address_occupied_target_is_409() {
  UnitFacts facts[UNITS_AMOUNT];
  fillFacts(facts);
  // Address 5 answers (bootloader counts as occupied) — moving 3 onto it collides.
  TEST_ASSERT_EQUAL(409, maintValidateSetAddressTarget(5, 3, facts, UNITS_AMOUNT).httpStatus);
}

static void test_set_address_burning_current_address_is_allowed() {
  UnitFacts facts[UNITS_AMOUNT];
  fillFacts(facts);
  // The bulk-migration case: target == source, occupied by the unit itself.
  TEST_ASSERT_EQUAL(200, maintValidateSetAddressTarget(3, 3, facts, UNITS_AMOUNT).httpStatus);
}

static void test_set_address_free_target_passes() {
  UnitFacts facts[UNITS_AMOUNT];
  fillFacts(facts);
  TEST_ASSERT_EQUAL(200, maintValidateSetAddressTarget(7, 3, facts, UNITS_AMOUNT).httpStatus);
}

// --- wire-byte encoders (negative payloads must encode exactly like v1) --------

static void test_offset_encodes_int16_le_negative() {
  uint8_t buf[2];
  maintEncodeOffsetLE(-1200, buf);  // 0xFB50
  TEST_ASSERT_EQUAL_HEX8(0x50, buf[0]);
  TEST_ASSERT_EQUAL_HEX8(0xFB, buf[1]);
  maintEncodeOffsetLE(300, buf);  // 0x012C
  TEST_ASSERT_EQUAL_HEX8(0x2C, buf[0]);
  TEST_ASSERT_EQUAL_HEX8(0x01, buf[1]);
}

static void test_jog_encodes_signed_byte() {
  TEST_ASSERT_EQUAL_HEX8(0xFB, maintEncodeJogByte(-5));
  TEST_ASSERT_EQUAL_HEX8(0x7F, maintEncodeJogByte(127));
  TEST_ASSERT_EQUAL_HEX8(0x05, maintEncodeJogByte(5));
}

// --- post-probe postcondition classifiers (compound address ops) ---------------

static void test_set_address_ok_when_target_answers_sketch() {
  UnitFacts facts[UNITS_AMOUNT];
  fillFacts(facts);
  facts[6].state = 1;  // unit reappeared at address 7
  MaintReason reason = MaintReason::None;
  TEST_ASSERT_EQUAL(MaintOutcome::Ok,
                    classifySetAddressOutcome(facts, UNITS_AMOUNT, 7, reason));
  TEST_ASSERT_EQUAL(MaintReason::None, reason);
}

static void test_set_address_fails_when_target_silent_after_reprobe() {
  UnitFacts facts[UNITS_AMOUNT];
  fillFacts(facts);
  MaintReason reason = MaintReason::None;
  TEST_ASSERT_EQUAL(MaintOutcome::PostconditionFail,
                    classifySetAddressOutcome(facts, UNITS_AMOUNT, 7, reason));
  TEST_ASSERT_EQUAL(MaintReason::UnitMissingAfterReprobe, reason);
}

static void test_clear_address_ok_when_unit_count_holds() {
  MaintReason reason = MaintReason::None;
  TEST_ASSERT_EQUAL(MaintOutcome::Ok, classifyClearAddressOutcome(5, 5, reason));
}

static void test_clear_address_fails_when_a_unit_vanished() {
  // DIP collision (or DIP beyond the managed range): the unit rejoined
  // somewhere the master can't see — recoverable by DIP + power cycle.
  MaintReason reason = MaintReason::None;
  TEST_ASSERT_EQUAL(MaintOutcome::PostconditionFail,
                    classifyClearAddressOutcome(5, 4, reason));
  TEST_ASSERT_EQUAL(MaintReason::UnitMissingAfterReprobe, reason);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_missing_address_is_400);
  RUN_TEST(test_unparsable_address_is_400);
  RUN_TEST(test_address_out_of_i2c_range_is_400);
  RUN_TEST(test_address_beyond_managed_units_is_404);
  RUN_TEST(test_protocol_mismatch_unit_is_409_not_drivable);
  RUN_TEST(test_unread_protocol_stays_drivable);
  RUN_TEST(test_silent_and_bootloader_units_are_404);
  RUN_TEST(test_sketch_unit_passes_and_yields_address);
  RUN_TEST(test_hex_address_parses_v1_strtol_base0);
  RUN_TEST(test_offset_limit_is_one_revolution_both_signs);
  RUN_TEST(test_jog_limit_is_int8_range);
  RUN_TEST(test_gates_accept_any_byte_and_reject_beyond_it);
  RUN_TEST(test_set_address_target_out_of_managed_range_is_400);
  RUN_TEST(test_set_address_occupied_target_is_409);
  RUN_TEST(test_set_address_burning_current_address_is_allowed);
  RUN_TEST(test_set_address_free_target_passes);
  RUN_TEST(test_offset_encodes_int16_le_negative);
  RUN_TEST(test_jog_encodes_signed_byte);
  RUN_TEST(test_set_address_ok_when_target_answers_sketch);
  RUN_TEST(test_set_address_fails_when_target_silent_after_reprobe);
  RUN_TEST(test_clear_address_ok_when_unit_count_holds);
  RUN_TEST(test_clear_address_fails_when_a_unit_vanished);
  return UNITY_END();
}
