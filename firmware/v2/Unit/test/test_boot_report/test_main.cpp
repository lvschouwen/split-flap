// Host-side tests for the GET_BOOT_INFO reply codec (BootUpdateReport.h, #499):
// the unit encodes its lock/fuse/boot-CRC/state/result into 11 checksummed
// bytes; both masters decode and validate them. Old firmware answers the unknown
// opcode with its 1-byte status reply + bus padding, so the checksum + range
// checks must reject that instead of "verifying" garbage (the #106 class).

#include <unity.h>
#include <stdint.h>
#include <string.h>
#include "BootUpdateReport.h"

void setUp() {}
void tearDown() {}

static BootUpdateReport sample() {
  BootUpdateReport r;
  r.lockByte = 0xFC;
  r.fuseLow = 0xFF;
  r.fuseHigh = 0xDC;
  r.fuseExt = 0xFD;
  r.bootCrc32 = 0x18173addUL;
  r.state = BOOT_STATE_OLD;
  r.lastResult = BOOT_RESULT_NONE;
  return r;
}

static void test_roundtrip() {
  uint8_t buf[BOOT_INFO_REPLY_LEN];
  BootUpdateReport in = sample();
  bootInfoEncode(in, buf);
  BootUpdateReport out;
  TEST_ASSERT_TRUE(bootInfoDecode(buf, out));
  TEST_ASSERT_EQUAL_HEX8(in.lockByte, out.lockByte);
  TEST_ASSERT_EQUAL_HEX8(in.fuseLow, out.fuseLow);
  TEST_ASSERT_EQUAL_HEX8(in.fuseHigh, out.fuseHigh);
  TEST_ASSERT_EQUAL_HEX8(in.fuseExt, out.fuseExt);
  TEST_ASSERT_EQUAL_HEX32(in.bootCrc32, out.bootCrc32);
  TEST_ASSERT_EQUAL(in.state, out.state);
  TEST_ASSERT_EQUAL(in.lastResult, out.lastResult);
}

static void test_crc_is_little_endian_in_wire() {
  uint8_t buf[BOOT_INFO_REPLY_LEN];
  BootUpdateReport in = sample();
  in.bootCrc32 = 0xAABBCCDDUL;
  bootInfoEncode(in, buf);
  TEST_ASSERT_EQUAL_HEX8(0xDD, buf[4]);
  TEST_ASSERT_EQUAL_HEX8(0xCC, buf[5]);
  TEST_ASSERT_EQUAL_HEX8(0xBB, buf[6]);
  TEST_ASSERT_EQUAL_HEX8(0xAA, buf[7]);
}

static void test_bad_checksum_rejected() {
  uint8_t buf[BOOT_INFO_REPLY_LEN];
  bootInfoEncode(sample(), buf);
  buf[BOOT_INFO_REPLY_LEN - 1] ^= 0xFF;
  BootUpdateReport out;
  TEST_ASSERT_FALSE(bootInfoDecode(buf, out));
}

static void test_all_ff_rejected() {
  // The classic old-firmware / bus-idle pattern must not decode.
  uint8_t buf[BOOT_INFO_REPLY_LEN];
  memset(buf, 0xFF, sizeof(buf));
  BootUpdateReport out;
  TEST_ASSERT_FALSE(bootInfoDecode(buf, out));
}

static void test_out_of_range_state_rejected() {
  uint8_t buf[BOOT_INFO_REPLY_LEN];
  BootUpdateReport in = sample();
  in.state = 99;  // not a BootSectionState
  bootInfoEncode(in, buf);  // checksum valid, but state is impossible
  BootUpdateReport out;
  TEST_ASSERT_FALSE(bootInfoDecode(buf, out));
}

static void test_out_of_range_result_rejected() {
  uint8_t buf[BOOT_INFO_REPLY_LEN];
  BootUpdateReport in = sample();
  in.lastResult = 99;
  bootInfoEncode(in, buf);
  BootUpdateReport out;
  TEST_ASSERT_FALSE(bootInfoDecode(buf, out));
}

// --- lock/fuse readability (#518) ---

static void test_unreadable_flag_roundtrips_without_disturbing_the_result() {
  for (int result = 0; result < BOOT_RESULT_COUNT; result++) {
    BootUpdateReport in = sample();
    in.lastResult = (uint8_t)result;
    in.lockFuseReadable = false;
    uint8_t buf[BOOT_INFO_REPLY_LEN];
    bootInfoEncode(in, buf);
    TEST_ASSERT_EQUAL_HEX8(result | BOOT_INFO_FLAG_LOCKFUSE_UNREADABLE, buf[9]);
    BootUpdateReport out;
    TEST_ASSERT_TRUE(bootInfoDecode(buf, out));
    TEST_ASSERT_FALSE(out.lockFuseReadable);
    TEST_ASSERT_EQUAL(result, out.lastResult);
    TEST_ASSERT_EQUAL(in.state, out.state);
    TEST_ASSERT_EQUAL_HEX32(in.bootCrc32, out.bootCrc32);
  }
}

// #552: where lock and fuses are unreadable, three of their four bytes carry
// what the signature-row read returned. The lock byte stays a placeholder.
static void test_unreadable_report_carries_the_signature_row_read() {
  BootUpdateReport in = sample();
  in.lockFuseReadable = false;
  in.sigRow[0] = 0x1E;
  in.sigRow[1] = 0x95;
  in.sigRow[2] = 0x0F;
  uint8_t buf[BOOT_INFO_REPLY_LEN];
  bootInfoEncode(in, buf);
  TEST_ASSERT_EQUAL_HEX8(0xFF, buf[0]);
  TEST_ASSERT_EQUAL_HEX8(0x1E, buf[1]);
  TEST_ASSERT_EQUAL_HEX8(0x95, buf[2]);
  TEST_ASSERT_EQUAL_HEX8(0x0F, buf[3]);
  BootUpdateReport out;
  TEST_ASSERT_TRUE(bootInfoDecode(buf, out));
  TEST_ASSERT_FALSE(out.lockFuseReadable);
  TEST_ASSERT_TRUE(bootSigRowReported(out));
  TEST_ASSERT_TRUE(bootSigRowIsAtmega328p(out));
  // The signature bytes never read as lock or fuses.
  TEST_ASSERT_EQUAL_HEX8(0xFF, out.lockByte);
  TEST_ASSERT_EQUAL_HEX8(0xFF, out.fuseLow);
  TEST_ASSERT_EQUAL_HEX8(0xFF, out.fuseHigh);
  TEST_ASSERT_EQUAL_HEX8(0xFF, out.fuseExt);
  TEST_ASSERT_EQUAL_HEX8(0xFF, bootEffectiveLockByte(out));
}

// A unit built before #552 sends 0xFF placeholders there: nothing reported.
static void test_placeholders_are_not_a_signature_read() {
  BootUpdateReport in = sample();
  in.lockFuseReadable = false;
  uint8_t buf[BOOT_INFO_REPLY_LEN];
  bootInfoEncode(in, buf);
  BootUpdateReport out;
  TEST_ASSERT_TRUE(bootInfoDecode(buf, out));
  TEST_ASSERT_FALSE(bootSigRowReported(out));
  TEST_ASSERT_FALSE(bootSigRowIsAtmega328p(out));
}

// A unit that reads its fuses reports them, and no signature read.
static void test_readable_report_carries_no_signature_row() {
  BootUpdateReport in = sample();
  in.sigRow[0] = 0x1E;  // set, but not what this report is for
  uint8_t buf[BOOT_INFO_REPLY_LEN];
  bootInfoEncode(in, buf);
  TEST_ASSERT_EQUAL_HEX8(in.lockByte, buf[0]);
  TEST_ASSERT_EQUAL_HEX8(in.fuseLow, buf[1]);
  BootUpdateReport out;
  TEST_ASSERT_TRUE(bootInfoDecode(buf, out));
  TEST_ASSERT_TRUE(out.lockFuseReadable);
  TEST_ASSERT_FALSE(bootSigRowReported(out));
}

// The chip answered the signature-row read with program bytes too.
static void test_a_fallen_through_signature_read_is_not_the_signature() {
  BootUpdateReport r = sample();
  r.lockFuseReadable = false;
  r.sigRow[0] = 0x0C;
  r.sigRow[1] = 0xA2;
  r.sigRow[2] = 0x0C;
  TEST_ASSERT_TRUE(bootSigRowReported(r));
  TEST_ASSERT_FALSE(bootSigRowIsAtmega328p(r));
}

static void test_readable_report_is_byte_identical_to_the_flagless_format() {
  // A unit that reads its fuses sends exactly what it sent before the flag
  // existed, so a master predating it still decodes that unit.
  BootUpdateReport in = sample();
  in.lastResult = BOOT_RESULT_STAGE2_OK;
  uint8_t buf[BOOT_INFO_REPLY_LEN];
  bootInfoEncode(in, buf);
  TEST_ASSERT_EQUAL_HEX8(BOOT_RESULT_STAGE2_OK, buf[9]);
  BootUpdateReport out;
  TEST_ASSERT_TRUE(bootInfoDecode(buf, out));
  TEST_ASSERT_TRUE(out.lockFuseReadable);
}

static void test_flag_does_not_admit_an_out_of_range_result() {
  uint8_t buf[BOOT_INFO_REPLY_LEN];
  bootInfoEncode(sample(), buf);
  buf[9] = (uint8_t)(BOOT_RESULT_COUNT | BOOT_INFO_FLAG_LOCKFUSE_UNREADABLE);
  uint8_t x = 0;
  for (uint8_t i = 0; i < BOOT_INFO_REPLY_LEN - 1; i++) x ^= buf[i];
  buf[BOOT_INFO_REPLY_LEN - 1] = (uint8_t)(x ^ BOOT_INFO_CHECKSUM_MASK);
  BootUpdateReport out;
  TEST_ASSERT_FALSE(bootInfoDecode(buf, out));
}

// What 11 of the 21 fielded units returned: the reset vector, read back as
// lock 94, lfuse 0c, hfuse 02, efuse a2.
static void test_fall_through_is_recognised_from_the_wall_values() {
  const uint8_t flash0to3[4] = {0x0C, 0x94, 0xA2, 0x02};  // jmp 0x02A2
  TEST_ASSERT_TRUE(bootLockFuseReadFellThrough(0x94, 0x0C, 0x02, 0xA2, flash0to3));
  // The other 10 units: real values, with the same sketch in flash.
  TEST_ASSERT_FALSE(bootLockFuseReadFellThrough(0xFF, 0xFF, 0xDC, 0xFD, flash0to3));
}

static void test_fall_through_needs_all_four_bytes_to_match() {
  const uint8_t flash0to3[4] = {0x0C, 0x94, 0xA2, 0x02};
  // One coincidence is not a fall-through — e.g. a real lock byte that happens
  // to equal flash byte 1 while the fuses are real.
  TEST_ASSERT_FALSE(bootLockFuseReadFellThrough(0x94, 0xFF, 0xDC, 0xFD, flash0to3));
  TEST_ASSERT_FALSE(bootLockFuseReadFellThrough(0xFF, 0x0C, 0x02, 0xA2, flash0to3));
  TEST_ASSERT_FALSE(bootLockFuseReadFellThrough(0x94, 0x0C, 0x02, 0xFD, flash0to3));
  TEST_ASSERT_FALSE(bootLockFuseReadFellThrough(0x94, 0x0C, 0xDC, 0xA2, flash0to3));
  // Each byte is compared against ITS Z address, not just any of the four.
  TEST_ASSERT_FALSE(bootLockFuseReadFellThrough(0x0C, 0x94, 0xA2, 0x02, flash0to3));
}

static void test_effective_lock_byte() {
  BootUpdateReport r = sample();
  r.lockByte = 0x00;  // readable and closed
  TEST_ASSERT_EQUAL_HEX8(0x00, bootEffectiveLockByte(r));
  TEST_ASSERT_FALSE(bootLockPermitsBootWrite(bootEffectiveLockByte(r)));
  r.lockFuseReadable = false;  // unknown: proceed, verification is the gate
  TEST_ASSERT_EQUAL_HEX8(0xFF, bootEffectiveLockByte(r));
  TEST_ASSERT_TRUE(bootLockPermitsBootWrite(bootEffectiveLockByte(r)));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_roundtrip);
  RUN_TEST(test_crc_is_little_endian_in_wire);
  RUN_TEST(test_bad_checksum_rejected);
  RUN_TEST(test_all_ff_rejected);
  RUN_TEST(test_out_of_range_state_rejected);
  RUN_TEST(test_out_of_range_result_rejected);
  RUN_TEST(test_unreadable_flag_roundtrips_without_disturbing_the_result);
  RUN_TEST(test_unreadable_report_carries_the_signature_row_read);
  RUN_TEST(test_placeholders_are_not_a_signature_read);
  RUN_TEST(test_readable_report_carries_no_signature_row);
  RUN_TEST(test_a_fallen_through_signature_read_is_not_the_signature);
  RUN_TEST(test_readable_report_is_byte_identical_to_the_flagless_format);
  RUN_TEST(test_flag_does_not_admit_an_out_of_range_result);
  RUN_TEST(test_fall_through_is_recognised_from_the_wall_values);
  RUN_TEST(test_fall_through_needs_all_four_bytes_to_match);
  RUN_TEST(test_effective_lock_byte);
  return UNITY_END();
}
