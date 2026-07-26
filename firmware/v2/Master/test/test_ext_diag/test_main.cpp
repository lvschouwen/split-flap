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

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_roundtrip);
  RUN_TEST(test_rejects_all_ff);
  RUN_TEST(test_rejects_all_zero);
  RUN_TEST(test_rejects_bitflip);
  return UNITY_END();
}
