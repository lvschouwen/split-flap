// Host-side unit tests for the pure ext-diag logic in UnitExtDiag.h (#365):
// the checksummed CMD_GET_EXT_DIAG wire packet (step-excess, Vcc sag, hall
// edges/rev, duty window, status bits). The AVR-side measurement glue that
// feeds it is bench tier.

#include <unity.h>
#include <stdint.h>
#include "UnitExtDiag.h"

void setUp() {}
void tearDown() {}

static void test_roundtrip() {
  UnitExtDiag in;
  in.stepExcessLast = 40;
  in.stepExcessMax = 512;
  in.vccSagLastMove = 4321;
  in.hallEdgesLastRev = 1;
  in.dutyWindow = 77;
  in.statusBits = EXT_DIAG_STATUS_STALL;
  uint8_t buf[EXT_DIAG_REPLY_LEN];
  extDiagEncodeReply(in, buf);

  UnitExtDiag out;
  TEST_ASSERT_TRUE(extDiagReadbackValid(buf, out));
  TEST_ASSERT_EQUAL_UINT16(40, out.stepExcessLast);
  TEST_ASSERT_EQUAL_UINT16(512, out.stepExcessMax);
  TEST_ASSERT_EQUAL_UINT16(4321, out.vccSagLastMove);
  TEST_ASSERT_EQUAL_UINT8(1, out.hallEdgesLastRev);
  TEST_ASSERT_EQUAL_UINT16(77, out.dutyWindow);
  TEST_ASSERT_EQUAL_UINT8(EXT_DIAG_STATUS_STALL, out.statusBits);
}

static void test_rejects_all_ff() {
  // Un-flashed unit -> un-ACKed read padding (0xFF from the bus).
  uint8_t buf[EXT_DIAG_REPLY_LEN];
  for (auto& b : buf) b = 0xFF;
  UnitExtDiag out;
  TEST_ASSERT_FALSE(extDiagReadbackValid(buf, out));
}

static void test_rejects_all_zero() {
  uint8_t buf[EXT_DIAG_REPLY_LEN] = {0};
  UnitExtDiag out;
  TEST_ASSERT_FALSE(extDiagReadbackValid(buf, out));
}

static void test_rejects_bitflip() {
  UnitExtDiag in;
  uint8_t buf[EXT_DIAG_REPLY_LEN];
  extDiagEncodeReply(in, buf);
  buf[3] ^= 0x20;
  UnitExtDiag out;
  TEST_ASSERT_FALSE(extDiagReadbackValid(buf, out));
}

// --- link-health extension (#502) ---

static void test_link_roundtrip() {
  UnitLinkStats in;
  in.uptimeSeconds = 0x01234567UL;  // past the u16 GET_STATUS ceiling
  in.rxFrames = 0xBEEF;
  in.txReplies = 0x1234;
  in.deafHeals = 3;
  uint8_t ext[EXT_DIAG_LINK_EXT_LEN];
  extDiagLinkEncode(in, ext);
  UnitLinkStats out;
  TEST_ASSERT_TRUE(extDiagLinkReadbackValid(ext, out));
  TEST_ASSERT_EQUAL_UINT32(0x01234567UL, out.uptimeSeconds);
  TEST_ASSERT_EQUAL_UINT16(0xBEEF, out.rxFrames);
  TEST_ASSERT_EQUAL_UINT16(0x1234, out.txReplies);
  TEST_ASSERT_EQUAL_UINT8(3, out.deafHeals);
}

static void test_link_rejects_bus_padding_and_zero() {
  // A unit without the extension stops driving after the base packet.
  uint8_t ext[EXT_DIAG_LINK_EXT_LEN];
  for (auto& b : ext) b = 0xFF;
  UnitLinkStats out;
  out.uptimeSeconds = 77;
  TEST_ASSERT_FALSE(extDiagLinkReadbackValid(ext, out));
  for (auto& b : ext) b = 0x00;
  TEST_ASSERT_FALSE(extDiagLinkReadbackValid(ext, out));
  TEST_ASSERT_EQUAL_UINT32(77, out.uptimeSeconds);  // untouched on rejection
}

static void test_link_rejects_every_single_bit_flip() {
  UnitLinkStats in;
  in.uptimeSeconds = 90000;
  in.rxFrames = 512;
  uint8_t ext[EXT_DIAG_LINK_EXT_LEN];
  extDiagLinkEncode(in, ext);
  for (uint8_t i = 0; i < EXT_DIAG_LINK_EXT_LEN; i++) {
    for (uint8_t bit = 0; bit < 8; bit++) {
      ext[i] ^= (uint8_t)(1 << bit);
      UnitLinkStats out;
      TEST_ASSERT_FALSE(extDiagLinkReadbackValid(ext, out));
      ext[i] ^= (uint8_t)(1 << bit);
    }
  }
}

static void test_link_extension_leaves_the_base_packet_alone() {
  // The base reply and its checksum are what every fielded master parses.
  TEST_ASSERT_EQUAL_INT(11, EXT_DIAG_REPLY_LEN);
  TEST_ASSERT_EQUAL_INT(21, EXT_DIAG_LINK_REPLY_LEN);
  TEST_ASSERT_NOT_EQUAL(EXT_DIAG_REPLY_CHECKSUM_MASK, EXT_DIAG_LINK_CHECKSUM_MASK);
  TEST_ASSERT_TRUE(EXT_DIAG_LINK_REPLY_LEN <= 32);  // AVR Wire slave tx buffer
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_roundtrip);
  RUN_TEST(test_rejects_all_ff);
  RUN_TEST(test_rejects_all_zero);
  RUN_TEST(test_rejects_bitflip);
  RUN_TEST(test_link_roundtrip);
  RUN_TEST(test_link_rejects_bus_padding_and_zero);
  RUN_TEST(test_link_rejects_every_single_bit_flip);
  RUN_TEST(test_link_extension_leaves_the_base_packet_alone);
  return UNITY_END();
}
