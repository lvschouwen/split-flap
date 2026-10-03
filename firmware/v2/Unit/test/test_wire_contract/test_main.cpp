// Host-side unit tests for UnitWireContract.h (#405) — the core master<->unit
// reads and writes that predate the #231 checksum discipline and are brought
// into it here: GET_VERSION, GET_STATUS, GET_OFFSET, SET_OFFSET and
// SET_I2C_ADDRESS.
//
// The bus is not currently failing (i2cErr 0 across 10,604 transactions).
// These tests exist because a SILENT corruption on those paths would not have
// been detected, which makes "0 errors" weaker evidence than it looks.

#include <unity.h>
#include <stdint.h>
#include <string.h>
#include "UnitWireContract.h"

void setUp() {}
void tearDown() {}

// --- GET_VERSION (0x81) ---------------------------------------------------
// FIXED FOREVER: this reply is the cross-generation identity read, and it now
// carries the protocol version that gates every other opcode. Its format can
// never change again — a unit whose protocol version the master does not
// recognise is reachable through this opcode and ENTER_BOOTLOADER alone, so
// this packet has to stay parseable by every generation that will ever exist.

static void test_version_roundtrip() {
  uint8_t buf[VERSION_REPLY_LEN];
  versionEncodeReply("bf64943", 3, buf);

  UnitVersionPacket out;
  TEST_ASSERT_TRUE(versionReadbackValid(buf, VERSION_REPLY_LEN, out));
  TEST_ASSERT_EQUAL_STRING("bf64943", out.rev);
  TEST_ASSERT_EQUAL_UINT8(3, out.protocolVersion);
}

static void test_version_pads_short_revs_and_stays_terminated() {
  uint8_t buf[VERSION_REPLY_LEN];
  versionEncodeReply("abc", 1, buf);
  TEST_ASSERT_EQUAL_UINT8('a', buf[0]);
  TEST_ASSERT_EQUAL_UINT8(0, buf[3]);  // null-padded, not garbage
  UnitVersionPacket out;
  TEST_ASSERT_TRUE(versionReadbackValid(buf, VERSION_REPLY_LEN, out));
  TEST_ASSERT_EQUAL_STRING("abc", out.rev);
}

static void test_version_full_eight_char_rev_is_terminated() {
  // Eight rev chars exactly fill the field; the struct must still hand back a
  // usable C string rather than running into protocolVersion.
  uint8_t buf[VERSION_REPLY_LEN];
  versionEncodeReply("0123456789", 1, buf);  // over-long input is truncated
  UnitVersionPacket out;
  TEST_ASSERT_TRUE(versionReadbackValid(buf, VERSION_REPLY_LEN, out));
  TEST_ASSERT_EQUAL_STRING("01234567", out.rev);
  TEST_ASSERT_EQUAL_UINT8(1, out.protocolVersion);
}

static void test_version_rejects_corruption() {
  for (uint8_t i = 0; i < VERSION_REPLY_LEN; i++) {
    uint8_t buf[VERSION_REPLY_LEN];
    versionEncodeReply("bf64943", 1, buf);
    buf[i] ^= 0x01;
    UnitVersionPacket out;
    TEST_ASSERT_FALSE(versionReadbackValid(buf, VERSION_REPLY_LEN, out));
  }
}

static void test_version_rejects_bus_padding_shapes() {
  const uint8_t fills[] = {0x00, 0x01, 0xFF};
  for (uint8_t f = 0; f < 3; f++) {
    uint8_t buf[VERSION_REPLY_LEN];
    memset(buf, fills[f], sizeof(buf));
    UnitVersionPacket out;
    TEST_ASSERT_FALSE(versionReadbackValid(buf, VERSION_REPLY_LEN, out));
  }
}

static void test_version_rejects_short_reply() {
  uint8_t buf[VERSION_REPLY_LEN];
  versionEncodeReply("bf64943", 1, buf);
  UnitVersionPacket out;
  TEST_ASSERT_FALSE(versionReadbackValid(buf, VERSION_REPLY_LEN - 1, out));
}

// --- the protocol-version gate --------------------------------------------
// A unit whose version the master does not recognise must stay REACHABLE.
// Ignoring it outright would take a wall of newer units under an older master
// dark with no way back, because the master drives the reflash.

static void test_gate_accepts_the_matching_version() {
  TEST_ASSERT_TRUE(unitProtocolSupported(SFP_PROTOCOL_VERSION));
}

static void test_gate_rejects_any_other_version() {
  TEST_ASSERT_FALSE(unitProtocolSupported(SFP_PROTOCOL_VERSION + 1));
  TEST_ASSERT_FALSE(unitProtocolSupported(0));
  TEST_ASSERT_FALSE(unitProtocolSupported(0xFF));
}

static void test_gate_leaves_the_recovery_pair_open() {
  // The two opcodes documented FIXED FOREVER are the whole recovery path.
  TEST_ASSERT_TRUE(unitOpcodeAllowedWhenUnsupported(SFP_CMD_GET_VERSION));
  TEST_ASSERT_TRUE(unitOpcodeAllowedWhenUnsupported(SFP_CMD_ENTER_BOOTLOADER));
}

static void test_gate_blocks_everything_else() {
  const uint8_t blocked[] = {
      SFP_CMD_GET_STATUS,   SFP_CMD_GET_OFFSET,  SFP_CMD_GET_LETTER,
      SFP_CMD_GET_ODOMETER, SFP_CMD_GET_DIAG,    SFP_CMD_GET_SELF_TEST,
      SFP_CMD_GET_VITALS,   SFP_CMD_GET_EXT_DIAG, SFP_CMD_GET_LIFETIME,
      SFP_CMD_HOME,         SFP_CMD_JOG,         SFP_CMD_REBOOT,
      SFP_CMD_SET_OFFSET,   SFP_CMD_SET_I2C_ADDRESS,
      SFP_CMD_CLEAR_I2C_ADDRESS, SFP_CMD_IDENTIFY,
      SFP_CMD_RESET_ODOMETER, SFP_CMD_START_SELF_TEST,
  };
  for (uint8_t i = 0; i < sizeof(blocked); i++) {
    TEST_ASSERT_FALSE(unitOpcodeAllowedWhenUnsupported(blocked[i]));
  }
  // A letter index is a render, which is the loudest thing we could get
  // wrong against an unknown contract.
  TEST_ASSERT_FALSE(unitOpcodeAllowedWhenUnsupported(0));
  TEST_ASSERT_FALSE(unitOpcodeAllowedWhenUnsupported(44));
}

// --- GET_STATUS (0x83) ----------------------------------------------------
// The highest-frequency read on the bus. It feeds fault flags, lifetime reset
// counts, uptime, stale detection, the event log and the HA sensors, and it
// predates the checksum discipline entirely.

static void test_status_roundtrip() {
  const uint8_t payload[STATUS_PAYLOAD_LEN] = {0x21, 0x02, 3, 4, 0x12, 0x34, 5, 123};
  uint8_t buf[STATUS_REPLY_LEN];
  statusEncodeReply(payload, buf);

  uint8_t out[STATUS_PAYLOAD_LEN] = {0};
  TEST_ASSERT_TRUE(statusReadbackValid(buf, STATUS_REPLY_LEN, out));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(payload, out, STATUS_PAYLOAD_LEN);
}

static void test_status_rejects_corruption() {
  const uint8_t payload[STATUS_PAYLOAD_LEN] = {0x21, 0x02, 3, 4, 0x12, 0x34, 5, 123};
  for (uint8_t i = 0; i < STATUS_REPLY_LEN; i++) {
    uint8_t buf[STATUS_REPLY_LEN];
    statusEncodeReply(payload, buf);
    buf[i] ^= 0x01;
    uint8_t out[STATUS_PAYLOAD_LEN] = {0};
    TEST_ASSERT_FALSE(statusReadbackValid(buf, STATUS_REPLY_LEN, out));
  }
}

static void test_status_a_flipped_fault_bit_is_caught() {
  // The concrete harm: a corrupted flags byte invents a fault or masks a real
  // one, and nothing downstream would know.
  const uint8_t payload[STATUS_PAYLOAD_LEN] = {0x00, 0, 0, 0, 0, 0, 0, 0};
  uint8_t buf[STATUS_REPLY_LEN];
  statusEncodeReply(payload, buf);
  buf[0] |= 0x02;  // fabricate UNIT_FLAG_LAST_HOME_FAILED
  uint8_t out[STATUS_PAYLOAD_LEN] = {0};
  TEST_ASSERT_FALSE(statusReadbackValid(buf, STATUS_REPLY_LEN, out));
}

static void test_status_rejects_bus_padding_shapes() {
  const uint8_t fills[] = {0x00, 0x01, 0xFF};
  for (uint8_t f = 0; f < 3; f++) {
    uint8_t buf[STATUS_REPLY_LEN];
    memset(buf, fills[f], sizeof(buf));
    uint8_t out[STATUS_PAYLOAD_LEN] = {0};
    TEST_ASSERT_FALSE(statusReadbackValid(buf, STATUS_REPLY_LEN, out));
  }
}

static void test_status_rejection_leaves_output_untouched() {
  uint8_t buf[STATUS_REPLY_LEN];
  memset(buf, 0xFF, sizeof(buf));
  uint8_t out[STATUS_PAYLOAD_LEN];
  memset(out, 0x5A, sizeof(out));
  TEST_ASSERT_FALSE(statusReadbackValid(buf, STATUS_REPLY_LEN, out));
  for (uint8_t i = 0; i < STATUS_PAYLOAD_LEN; i++) {
    TEST_ASSERT_EQUAL_UINT8(0x5A, out[i]);
  }
}

// --- GET_OFFSET (0x82) ----------------------------------------------------

static void test_offset_roundtrip_signed() {
  uint8_t buf[OFFSET_REPLY_LEN];
  offsetEncodeReply(-1234, buf);
  int16_t out = 0;
  TEST_ASSERT_TRUE(offsetReadbackValid(buf, OFFSET_REPLY_LEN, out));
  TEST_ASSERT_EQUAL_INT16(-1234, out);
}

static void test_offset_rejects_corruption() {
  for (uint8_t i = 0; i < OFFSET_REPLY_LEN; i++) {
    uint8_t buf[OFFSET_REPLY_LEN];
    offsetEncodeReply(69, buf);
    buf[i] ^= 0x01;
    int16_t out = 0;
    TEST_ASSERT_FALSE(offsetReadbackValid(buf, OFFSET_REPLY_LEN, out));
  }
}

static void test_offset_zero_is_distinguishable_from_padding() {
  // 0 is a legitimate offset (uncalibrated), so its packet must not look like
  // a 0x00-padded reply.
  uint8_t buf[OFFSET_REPLY_LEN];
  offsetEncodeReply(0, buf);
  TEST_ASSERT_EQUAL_UINT8(OFFSET_REPLY_CHECKSUM_MASK, buf[OFFSET_REPLY_LEN - 1]);
  int16_t out = 42;
  TEST_ASSERT_TRUE(offsetReadbackValid(buf, OFFSET_REPLY_LEN, out));
  TEST_ASSERT_EQUAL_INT16(0, out);
}

// --- SET_OFFSET (0x93) ----------------------------------------------------
// Was fire-and-forget. Value plus bitwise complement in one transaction, the
// GET_LETTER idiom, so the unit rejects a disagreeing pair rather than
// persisting a corrupted calibration.

static void test_set_offset_roundtrip() {
  uint8_t buf[SET_OFFSET_PAYLOAD_LEN];
  setOffsetEncode(-2038, buf);
  int16_t out = 0;
  TEST_ASSERT_TRUE(setOffsetDecode(buf, SET_OFFSET_PAYLOAD_LEN, out));
  TEST_ASSERT_EQUAL_INT16(-2038, out);
}

static void test_set_offset_rejects_a_disagreeing_pair() {
  for (uint8_t i = 0; i < SET_OFFSET_PAYLOAD_LEN; i++) {
    uint8_t buf[SET_OFFSET_PAYLOAD_LEN];
    setOffsetEncode(69, buf);
    buf[i] ^= 0x01;
    int16_t out = 0;
    TEST_ASSERT_FALSE(setOffsetDecode(buf, SET_OFFSET_PAYLOAD_LEN, out));
  }
}

static void test_set_offset_rejects_short_payload() {
  uint8_t buf[SET_OFFSET_PAYLOAD_LEN];
  setOffsetEncode(69, buf);
  int16_t out = 0;
  TEST_ASSERT_FALSE(setOffsetDecode(buf, SET_OFFSET_PAYLOAD_LEN - 1, out));
}

static void test_set_offset_zero_survives_the_complement_check() {
  // 0 / ~0 is the pair most likely to be confused with an idle bus.
  uint8_t buf[SET_OFFSET_PAYLOAD_LEN];
  setOffsetEncode(0, buf);
  int16_t out = 99;
  TEST_ASSERT_TRUE(setOffsetDecode(buf, SET_OFFSET_PAYLOAD_LEN, out));
  TEST_ASSERT_EQUAL_INT16(0, out);
  // An all-zero payload is NOT a valid "set offset to 0" — the complement
  // half would have to be 0xFFFF.
  uint8_t zeros[SET_OFFSET_PAYLOAD_LEN] = {0, 0, 0, 0};
  TEST_ASSERT_FALSE(setOffsetDecode(zeros, SET_OFFSET_PAYLOAD_LEN, out));
}

// --- SET_I2C_ADDRESS (0x94) -----------------------------------------------
// The worst failure mode in the contract: one unprotected byte, after which
// the unit persists it and reboots. A corruption landing inside 1..126
// silently relocates the unit to an address nobody is looking at, and it
// cannot be verified afterwards because the unit is gone from the address you
// were talking to. Recovery is a physical trip to re-DIP.

static void test_set_address_roundtrip() {
  uint8_t buf[SET_ADDRESS_PAYLOAD_LEN];
  setAddressEncode(0x0F, buf);
  uint8_t out = 0;
  TEST_ASSERT_TRUE(setAddressDecode(buf, SET_ADDRESS_PAYLOAD_LEN, out));
  TEST_ASSERT_EQUAL_UINT8(0x0F, out);
}

static void test_set_address_rejects_a_disagreeing_pair() {
  for (uint8_t i = 0; i < SET_ADDRESS_PAYLOAD_LEN; i++) {
    uint8_t buf[SET_ADDRESS_PAYLOAD_LEN];
    setAddressEncode(0x0F, buf);
    buf[i] ^= 0x01;
    uint8_t out = 0;
    TEST_ASSERT_FALSE(setAddressDecode(buf, SET_ADDRESS_PAYLOAD_LEN, out));
  }
}

static void test_set_address_single_bit_corruption_within_range_is_caught() {
  // The exact scenario: 0x0F corrupts to 0x0E, still a legal address, and the
  // old contract would have persisted it and rebooted the unit out of reach.
  uint8_t buf[SET_ADDRESS_PAYLOAD_LEN];
  setAddressEncode(0x0F, buf);
  buf[0] = 0x0E;
  uint8_t out = 0;
  TEST_ASSERT_FALSE(setAddressDecode(buf, SET_ADDRESS_PAYLOAD_LEN, out));
}

static void test_set_address_rejects_short_payload() {
  uint8_t buf[SET_ADDRESS_PAYLOAD_LEN];
  setAddressEncode(0x0F, buf);
  uint8_t out = 0;
  TEST_ASSERT_FALSE(setAddressDecode(buf, SET_ADDRESS_PAYLOAD_LEN - 1, out));
}

// --- SET_GATES (0x99) -----------------------------------------------------
// The write half of #406's feature-gate byte (#409). Same complement pair as
// the two writes above, and the only one of the three that can be verified
// afterwards — GET_LIFETIME reports the byte back under a checksum.

static void test_set_gates_roundtrip() {
  uint8_t buf[SET_GATES_PAYLOAD_LEN];
  setGatesEncode(0x01, buf);
  uint8_t out = 0xFF;
  TEST_ASSERT_TRUE(setGatesDecode(buf, SET_GATES_PAYLOAD_LEN, out));
  TEST_ASSERT_EQUAL_UINT8(0x01, out);
}

// Turning every gate back OFF is the safety action — 0x00 followed by 0xFF,
// which an idle or stuck bus cannot spell either way round.
static void test_set_gates_can_clear_every_gate() {
  uint8_t buf[SET_GATES_PAYLOAD_LEN];
  setGatesEncode(0x00, buf);
  TEST_ASSERT_EQUAL_UINT8(0x00, buf[0]);
  TEST_ASSERT_EQUAL_UINT8(0xFF, buf[1]);
  uint8_t out = 0xAA;
  TEST_ASSERT_TRUE(setGatesDecode(buf, SET_GATES_PAYLOAD_LEN, out));
  TEST_ASSERT_EQUAL_UINT8(0x00, out);
}

static void test_set_gates_rejects_a_disagreeing_pair() {
  for (uint8_t i = 0; i < SET_GATES_PAYLOAD_LEN; i++) {
    uint8_t buf[SET_GATES_PAYLOAD_LEN];
    setGatesEncode(0x03, buf);
    buf[i] ^= 0x01;
    uint8_t out = 0;
    TEST_ASSERT_FALSE(setGatesDecode(buf, SET_GATES_PAYLOAD_LEN, out));
  }
}

// An all-zero and an all-0xFF bus both decode to nothing: 0x00/0x00 and
// 0xFF/0xFF are each a disagreeing pair.
static void test_set_gates_rejects_a_flatlined_bus() {
  uint8_t out = 0;
  const uint8_t zeros[SET_GATES_PAYLOAD_LEN] = {0x00, 0x00};
  TEST_ASSERT_FALSE(setGatesDecode(zeros, SET_GATES_PAYLOAD_LEN, out));
  const uint8_t ones[SET_GATES_PAYLOAD_LEN] = {0xFF, 0xFF};
  TEST_ASSERT_FALSE(setGatesDecode(ones, SET_GATES_PAYLOAD_LEN, out));
}

static void test_set_gates_rejects_short_payload() {
  uint8_t buf[SET_GATES_PAYLOAD_LEN];
  setGatesEncode(0x02, buf);
  uint8_t out = 0;
  TEST_ASSERT_FALSE(setGatesDecode(buf, SET_GATES_PAYLOAD_LEN - 1, out));
}

// --- masks ----------------------------------------------------------------

static void test_checksum_masks_are_all_distinct() {
  const uint8_t masks[] = {
      0xA5,  // odometer
      0xB7,  // diag
      0x5C,  // self-test
      0x3C,  // vitals
      0x93,  // ext-diag
      0x6E,  // lifetime
      VERSION_REPLY_CHECKSUM_MASK,
      STATUS_REPLY_CHECKSUM_MASK,
      OFFSET_REPLY_CHECKSUM_MASK,
  };
  for (uint8_t i = 0; i < sizeof(masks); i++) {
    TEST_ASSERT_TRUE(masks[i] != 0x00 && masks[i] != 0xFF);
    for (uint8_t j = (uint8_t)(i + 1); j < sizeof(masks); j++) {
      TEST_ASSERT_TRUE(masks[i] != masks[j]);
    }
  }
}

// --- no-argument mutations (#512) ---

static const uint8_t kNoArgMutations[] = {
    SFP_CMD_ENTER_BOOTLOADER,  SFP_CMD_HOME,     SFP_CMD_REBOOT,
    SFP_CMD_CLEAR_I2C_ADDRESS, SFP_CMD_IDENTIFY, SFP_CMD_RESET_ODOMETER,
    SFP_CMD_START_SELF_TEST,
};

static void test_noarg_guarded_form_is_accepted_in_both_modes() {
  for (uint8_t op : kNoArgMutations) {
    TEST_ASSERT_TRUE(sfpIsNoArgMutation(op));
    TEST_ASSERT_TRUE(noArgMutationAccepted(op, 1, noArgGuardByte(op), false));
    TEST_ASSERT_TRUE(noArgMutationAccepted(op, 1, noArgGuardByte(op), true));
  }
}

static void test_noarg_bare_form_is_accepted_only_when_not_strict() {
  for (uint8_t op : kNoArgMutations) {
    TEST_ASSERT_TRUE(noArgMutationAccepted(op, 0, 0, false));
    if (op == SFP_CMD_ENTER_BOOTLOADER) continue;
    TEST_ASSERT_FALSE(noArgMutationAccepted(op, 0, 0, true));
    // The ignored guard argument must not rescue a bare frame.
    TEST_ASSERT_FALSE(noArgMutationAccepted(op, 0, noArgGuardByte(op), true));
  }
}

// ENTER_BOOTLOADER's one-byte form is FIXED FOREVER (SplitFlapProtocol.h): a
// master of any generation must be able to push a strict unit into twiboot.
static void test_noarg_bare_enter_bootloader_survives_strict() {
  TEST_ASSERT_TRUE(unitOpcodeAllowedWhenUnsupported(SFP_CMD_ENTER_BOOTLOADER));
  TEST_ASSERT_TRUE(noArgMutationAccepted(SFP_CMD_ENTER_BOOTLOADER, 0, 0, true));
  // A trailing byte still has to be the guard: blank (0x00) with bit 7 flipped
  // is this opcode followed by a speed byte.
  TEST_ASSERT_FALSE(noArgMutationAccepted(SFP_CMD_ENTER_BOOTLOADER, 1, 12, true));
  TEST_ASSERT_FALSE(noArgMutationAccepted(SFP_CMD_ENTER_BOOTLOADER, 1, 12, false));
}

static void test_noarg_wrong_guard_or_long_frame_is_always_rejected() {
  for (uint8_t op : kNoArgMutations) {
    for (int strict = 0; strict <= 1; strict++) {
      for (int g = 0; g < 256; g++) {
        if ((uint8_t)g == noArgGuardByte(op)) continue;
        TEST_ASSERT_FALSE(noArgMutationAccepted(op, 1, (uint8_t)g, strict != 0));
      }
      TEST_ASSERT_FALSE(
          noArgMutationAccepted(op, 2, noArgGuardByte(op), strict != 0));
      TEST_ASSERT_FALSE(
          noArgMutationAccepted(op, 0xFF, noArgGuardByte(op), strict != 0));
    }
  }
}

// The failure this exists for: a two-byte letter write [index, speed] whose
// index takes one bit flip and becomes a mutation opcode. With the gate OFF it
// must already be refused at every speed a drum can run.
static void test_noarg_no_bit_flipped_letter_write_runs_a_mutation() {
  int exposed = 0;
  for (int letter = 0; letter < SFP_FLAP_AMOUNT; letter++) {
    for (int bit = 0; bit < 8; bit++) {
      uint8_t flipped = (uint8_t)(letter ^ (1 << bit));
      if (!sfpIsNoArgMutation(flipped)) continue;
      exposed++;
      for (int speed = 0; speed <= 100; speed++) {
        TEST_ASSERT_FALSE(
            noArgMutationAccepted(flipped, 1, (uint8_t)speed, false));
      }
    }
  }
  TEST_ASSERT_TRUE(exposed > 0);  // the hole is real: some letters land there
}

// Strict mode closes the other path: a bare GET_* poll one bit flip away from
// a bare mutation.
static void test_noarg_strict_refuses_a_bit_flipped_bare_poll() {
  int exposed = 0;
  for (int op = 0x80; op <= 0x8F; op++) {
    if (sfpIsNoArgMutation((uint8_t)op)) continue;  // ENTER_BOOTLOADER itself
    for (int bit = 0; bit < 8; bit++) {
      uint8_t flipped = (uint8_t)(op ^ (1 << bit));
      if (!sfpIsNoArgMutation(flipped)) continue;
      if (flipped == SFP_CMD_ENTER_BOOTLOADER) continue;  // the fixed-forever form
      exposed++;
      TEST_ASSERT_FALSE(noArgMutationAccepted(flipped, 0, 0, true));
    }
  }
  TEST_ASSERT_TRUE(exposed > 0);
}

static void test_noarg_set_is_exactly_the_payloadless_mutations() {
  // Reads and payload-carrying mutations have their own protection.
  for (int op = SFP_CMD_GET_VERSION; op <= 0x8F; op++) {
    TEST_ASSERT_FALSE(sfpIsNoArgMutation((uint8_t)op));
  }
  TEST_ASSERT_FALSE(sfpIsNoArgMutation(SFP_CMD_JOG));
  TEST_ASSERT_FALSE(sfpIsNoArgMutation(SFP_CMD_SET_OFFSET));
  TEST_ASSERT_FALSE(sfpIsNoArgMutation(SFP_CMD_SET_I2C_ADDRESS));
  TEST_ASSERT_FALSE(sfpIsNoArgMutation(SFP_CMD_SET_GATES));
  for (int letter = 0; letter < SFP_FLAP_AMOUNT; letter++) {
    TEST_ASSERT_FALSE(sfpIsNoArgMutation((uint8_t)letter));
  }
}

// --- JOG (#512) ---

static void test_jog_roundtrip_with_complement() {
  for (int v = -127; v <= 127; v++) {
    uint8_t buf[JOG_PAYLOAD_LEN];
    jogEncode((uint8_t)(int8_t)v, buf);
    for (int strict = 0; strict <= 1; strict++) {
      int8_t out = 99;
      TEST_ASSERT_TRUE(jogDecode(buf, JOG_PAYLOAD_LEN, strict != 0, out));
      TEST_ASSERT_EQUAL_INT8(v, out);
    }
  }
}

static void test_jog_single_byte_form_only_without_strict() {
  const uint8_t pay[JOG_PAYLOAD_LEN] = {(uint8_t)(int8_t)-5, 0};
  int8_t out = 99;
  TEST_ASSERT_TRUE(jogDecode(pay, 1, false, out));
  TEST_ASSERT_EQUAL_INT8(-5, out);
  // Strict: this is also the shape of a letter write whose index flipped onto
  // 0x91, the byte being its speed.
  out = 99;
  TEST_ASSERT_FALSE(jogDecode(pay, 1, true, out));
  TEST_ASSERT_EQUAL_INT8(99, out);  // untouched on rejection
}

static void test_jog_rejects_a_disagreeing_pair_and_odd_lengths() {
  uint8_t buf[JOG_PAYLOAD_LEN];
  jogEncode(12, buf);
  int8_t out = 99;
  for (int strict = 0; strict <= 1; strict++) {
    for (int bit = 0; bit < 8; bit++) {
      buf[1] ^= (uint8_t)(1 << bit);
      TEST_ASSERT_FALSE(jogDecode(buf, JOG_PAYLOAD_LEN, strict != 0, out));
      buf[1] ^= (uint8_t)(1 << bit);
    }
    TEST_ASSERT_FALSE(jogDecode(buf, 0, strict != 0, out));
    TEST_ASSERT_FALSE(jogDecode(buf, 3, strict != 0, out));
  }
  TEST_ASSERT_EQUAL_INT8(99, out);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_version_roundtrip);
  RUN_TEST(test_version_pads_short_revs_and_stays_terminated);
  RUN_TEST(test_version_full_eight_char_rev_is_terminated);
  RUN_TEST(test_version_rejects_corruption);
  RUN_TEST(test_version_rejects_bus_padding_shapes);
  RUN_TEST(test_version_rejects_short_reply);
  RUN_TEST(test_gate_accepts_the_matching_version);
  RUN_TEST(test_gate_rejects_any_other_version);
  RUN_TEST(test_gate_leaves_the_recovery_pair_open);
  RUN_TEST(test_gate_blocks_everything_else);
  RUN_TEST(test_status_roundtrip);
  RUN_TEST(test_status_rejects_corruption);
  RUN_TEST(test_status_a_flipped_fault_bit_is_caught);
  RUN_TEST(test_status_rejects_bus_padding_shapes);
  RUN_TEST(test_status_rejection_leaves_output_untouched);
  RUN_TEST(test_offset_roundtrip_signed);
  RUN_TEST(test_offset_rejects_corruption);
  RUN_TEST(test_offset_zero_is_distinguishable_from_padding);
  RUN_TEST(test_set_offset_roundtrip);
  RUN_TEST(test_set_offset_rejects_a_disagreeing_pair);
  RUN_TEST(test_set_offset_rejects_short_payload);
  RUN_TEST(test_set_offset_zero_survives_the_complement_check);
  RUN_TEST(test_set_address_roundtrip);
  RUN_TEST(test_set_address_rejects_a_disagreeing_pair);
  RUN_TEST(test_set_address_single_bit_corruption_within_range_is_caught);
  RUN_TEST(test_set_address_rejects_short_payload);
  RUN_TEST(test_set_gates_roundtrip);
  RUN_TEST(test_set_gates_can_clear_every_gate);
  RUN_TEST(test_set_gates_rejects_a_disagreeing_pair);
  RUN_TEST(test_set_gates_rejects_a_flatlined_bus);
  RUN_TEST(test_set_gates_rejects_short_payload);
  RUN_TEST(test_checksum_masks_are_all_distinct);
  RUN_TEST(test_noarg_guarded_form_is_accepted_in_both_modes);
  RUN_TEST(test_noarg_bare_form_is_accepted_only_when_not_strict);
  RUN_TEST(test_noarg_bare_enter_bootloader_survives_strict);
  RUN_TEST(test_noarg_wrong_guard_or_long_frame_is_always_rejected);
  RUN_TEST(test_noarg_no_bit_flipped_letter_write_runs_a_mutation);
  RUN_TEST(test_noarg_strict_refuses_a_bit_flipped_bare_poll);
  RUN_TEST(test_noarg_set_is_exactly_the_payloadless_mutations);
  RUN_TEST(test_jog_roundtrip_with_complement);
  RUN_TEST(test_jog_single_byte_form_only_without_strict);
  RUN_TEST(test_jog_rejects_a_disagreeing_pair_and_odd_lengths);
  UNITY_END();
  return 0;
}
