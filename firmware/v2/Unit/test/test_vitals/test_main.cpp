// Host-side unit tests for the pure vitals logic in UnitVitals.h (#306):
// the bandgap->mV conversion and the checksummed CMD_GET_VITALS wire packet.
// The AVR ADC / free-RAM glue that feeds it is bench tier.

#include <unity.h>
#include <stdint.h>
#include <string.h>
#include "UnitVitals.h"

void setUp() {}
void tearDown() {}

// --- unitVccFromAdc: bandgap ratio -------------------------------------

static void test_vcc_5v_rail() {
  // Vcc=5.00V -> ADC = 1100*1023/5000 = 225.
  TEST_ASSERT_UINT16_WITHIN(2, 5001, unitVccFromAdc(225));
}

static void test_vcc_3v3_rail() {
  // Vcc=3.30V -> ADC = 1100*1023/3300 = 341.
  TEST_ASSERT_UINT16_WITHIN(2, 3300, unitVccFromAdc(341));
}

static void test_vcc_adc_zero_guarded() {
  // A glitched 0 must not divide-by-zero; report 0 (an impossible rail the
  // caller can treat as "no reading").
  TEST_ASSERT_EQUAL_UINT16(0, unitVccFromAdc(0));
}

static void test_vcc_degenerate_low_end() {
  // ADC=1023 == Vcc at the bandgap (1.1V); still fits u16.
  TEST_ASSERT_EQUAL_UINT16(1100, unitVccFromAdc(1023));
}

// --- packet round-trip -------------------------------------------------

static void test_encode_decode_roundtrip() {
  UnitVitals in;
  in.vccNow_mV = 4980;
  in.vccMin_mV = 4210;
  in.cmdPos = 17;
  in.freeRamMin = 384;
  uint8_t buf[VITALS_REPLY_LEN];
  vitalsEncodeReply(in, buf);

  UnitVitals out;
  TEST_ASSERT_TRUE(vitalsReadbackValid(buf, out));
  TEST_ASSERT_EQUAL_UINT16(4980, out.vccNow_mV);
  TEST_ASSERT_EQUAL_UINT16(4210, out.vccMin_mV);
  TEST_ASSERT_EQUAL_UINT8(17, out.cmdPos);
  TEST_ASSERT_EQUAL_UINT16(384, out.freeRamMin);
}

// --- garbage rejection (pre-vitals firmware / bus noise) ---------------

static void test_reject_corrupted_byte() {
  UnitVitals in;
  in.vccNow_mV = 5000;
  in.vccMin_mV = 4300;
  in.cmdPos = 3;
  in.freeRamMin = 500;
  uint8_t buf[VITALS_REPLY_LEN];
  vitalsEncodeReply(in, buf);
  buf[2] ^= 0x40;  // flip a payload bit; checksum must now fail

  UnitVitals out;
  TEST_ASSERT_FALSE(vitalsReadbackValid(buf, out));
}

static void test_reject_all_ff() {
  // Un-ACked read of an unknown opcode -> all 0xFF.
  uint8_t buf[VITALS_REPLY_LEN];
  memset(buf, 0xFF, sizeof(buf));
  UnitVitals out;
  TEST_ASSERT_FALSE(vitalsReadbackValid(buf, out));
}

static void test_reject_all_zero() {
  uint8_t buf[VITALS_REPLY_LEN];
  memset(buf, 0x00, sizeof(buf));
  UnitVitals out;
  TEST_ASSERT_FALSE(vitalsReadbackValid(buf, out));
}

static void test_reject_repeated_status_byte() {
  // Old firmware streams its 1-byte status reply; a repeated-byte read must
  // not "verify" as a tiny reading.
  uint8_t buf[VITALS_REPLY_LEN];
  memset(buf, 0x01, sizeof(buf));
  UnitVitals out;
  TEST_ASSERT_FALSE(vitalsReadbackValid(buf, out));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_vcc_5v_rail);
  RUN_TEST(test_vcc_3v3_rail);
  RUN_TEST(test_vcc_adc_zero_guarded);
  RUN_TEST(test_vcc_degenerate_low_end);
  RUN_TEST(test_encode_decode_roundtrip);
  RUN_TEST(test_reject_corrupted_byte);
  RUN_TEST(test_reject_all_ff);
  RUN_TEST(test_reject_all_zero);
  RUN_TEST(test_reject_repeated_status_byte);
  return UNITY_END();
}
