// Host-side unit tests for RescueIdentity.h (#195) — trimmed COPY of
// Master's DeviceIdentity.h (repo convention: rescue shares no compiled
// code with Master). Pins the pieces rescue actually uses: name resolution
// (NVS name if valid, else chip-id default) and the "-rescue" AP suffix
// fitting the 32-byte SSID limit at max name length.

#include <ArduinoFake.h>
#include <unity.h>

#include "../../RescueIdentity.h"

void setUp() {}
void tearDown() {}

static void test_stored_valid_name_wins() {
  TEST_ASSERT_EQUAL_STRING(
      "kitchen",
      resolveDeviceName(true, "kitchen", "split-flap", 0x9a3c1f).c_str());
}

static void test_empty_stored_name_falls_back_to_chip_id() {
  TEST_ASSERT_EQUAL_STRING(
      "split-flap-9a3c1f",
      resolveDeviceName(true, "", "split-flap", 0x9a3c1f).c_str());
}

static void test_invalid_stored_name_falls_back() {
  TEST_ASSERT_EQUAL_STRING(
      "split-flap-9a3c1f",
      resolveDeviceName(true, "Bad Name!", "split-flap", 0x9a3c1f).c_str());
}

static void test_unreadable_store_falls_back() {
  TEST_ASSERT_EQUAL_STRING(
      "split-flap-9a3c1f",
      resolveDeviceName(false, "kitchen", "split-flap", 0x9a3c1f).c_str());
}

static void test_rescue_ssid_fits_at_max_name_length() {
  // 24-char name + "-rescue" must stay inside the 32-byte 802.11 SSID limit.
  String maxName = "abcdefghijklmnopqrstuvwx";  // 24 chars
  TEST_ASSERT_TRUE(isValidDeviceName(maxName));
  String ssid = maxName + AP_SUFFIX_RESCUE;
  TEST_ASSERT_TRUE(ssid.length() <= 32);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_stored_valid_name_wins);
  RUN_TEST(test_empty_stored_name_falls_back_to_chip_id);
  RUN_TEST(test_invalid_stored_name_falls_back);
  RUN_TEST(test_unreadable_store_falls_back);
  RUN_TEST(test_rescue_ssid_fits_at_max_name_length);
  return UNITY_END();
}
