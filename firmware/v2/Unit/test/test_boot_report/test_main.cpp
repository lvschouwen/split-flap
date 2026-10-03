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

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_roundtrip);
  RUN_TEST(test_crc_is_little_endian_in_wire);
  RUN_TEST(test_bad_checksum_rejected);
  RUN_TEST(test_all_ff_rejected);
  RUN_TEST(test_out_of_range_state_rejected);
  RUN_TEST(test_out_of_range_result_rejected);
  return UNITY_END();
}
