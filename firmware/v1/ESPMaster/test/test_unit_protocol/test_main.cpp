// Host-side unit tests for the pure protocol helpers in UnitProtocolHelpers.h
// (master<->unit I2C wire format logic, no Arduino deps).

#include <ArduinoFake.h>
#include <unity.h>
#include "../../UnitProtocolHelpers.h"

void setUp() {
  ArduinoFakeReset();
}
void tearDown() {}

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
  RUN_TEST(test_valid_pair_accepted);
  RUN_TEST(test_wrong_complement_rejected);
  RUN_TEST(test_out_of_range_index_rejected_even_with_valid_complement);
  return UNITY_END();
}
