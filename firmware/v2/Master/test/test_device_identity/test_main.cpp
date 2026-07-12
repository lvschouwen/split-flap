// Host-side tests for DeviceIdentity.h (#125).
//
// Pure-logic coverage of the per-device network identity: name validation
// rules, chip-id default composition, the EEPROM-name -> chip-id-default
// resolution order, and the SSID length invariants that let a 24-char
// device name compose with every AP suffix inside the 32-byte SSID limit.

#include <ArduinoFake.h>
#include <unity.h>

#include <cstring>

#include "../../DeviceIdentity.h"

using namespace fakeit;

void setUp() { ArduinoFakeReset(); }
void tearDown() {}

// --- normalization ---------------------------------------------------------

static void test_normalize_lowercases_input() {
  TEST_ASSERT_EQUAL_STRING("kitchen", normalizeDeviceName(String("Kitchen")).c_str());
  TEST_ASSERT_EQUAL_STRING("split-flap-2", normalizeDeviceName(String("Split-Flap-2")).c_str());
}

static void test_normalize_leaves_valid_input_untouched() {
  TEST_ASSERT_EQUAL_STRING("livingroom", normalizeDeviceName(String("livingroom")).c_str());
}

// --- validation matrix -------------------------------------------------------

static void test_valid_names_accepted() {
  TEST_ASSERT_TRUE(isValidDeviceName(String("kitchen")));
  TEST_ASSERT_TRUE(isValidDeviceName(String("split-flap-livingroom")));
  TEST_ASSERT_TRUE(isValidDeviceName(String("a")));
  TEST_ASSERT_TRUE(isValidDeviceName(String("display2")));
  TEST_ASSERT_TRUE(isValidDeviceName(String("42")));
}

static void test_empty_name_is_not_valid() {
  // Empty means "reset to chip-id default" at the API layer; the validator
  // itself rejects it so resolveDeviceName() falls through to the default.
  TEST_ASSERT_FALSE(isValidDeviceName(String("")));
}

static void test_leading_or_trailing_hyphen_rejected() {
  TEST_ASSERT_FALSE(isValidDeviceName(String("-kitchen")));
  TEST_ASSERT_FALSE(isValidDeviceName(String("kitchen-")));
  TEST_ASSERT_FALSE(isValidDeviceName(String("-")));
}

static void test_invalid_characters_rejected() {
  TEST_ASSERT_FALSE(isValidDeviceName(String("kit chen")));
  TEST_ASSERT_FALSE(isValidDeviceName(String("kitchen!")));
  TEST_ASSERT_FALSE(isValidDeviceName(String("kit_chen")));
  TEST_ASSERT_FALSE(isValidDeviceName(String("kitchen.local")));
  // Uppercase is invalid at the validator: callers normalize first.
  TEST_ASSERT_FALSE(isValidDeviceName(String("Kitchen")));
}

static void test_length_bounds() {
  String max(""); for (int i = 0; i < DEVICE_NAME_MAX_LEN; i++) max += 'a';
  String over = max + "a";
  TEST_ASSERT_EQUAL_INT(24, DEVICE_NAME_MAX_LEN);
  TEST_ASSERT_TRUE(isValidDeviceName(max));
  TEST_ASSERT_FALSE(isValidDeviceName(over));
}

// --- chip-id default ---------------------------------------------------------

static void test_default_name_composes_prefix_and_hex_chip_id() {
  TEST_ASSERT_EQUAL_STRING("split-flap-9a3c1f", defaultDeviceName("split-flap", 0x9a3c1fUL).c_str());
}

static void test_default_name_fits_limit_for_max_chip_id() {
  // "split-flap-" (11) + 8 hex chars = 19 <= 24, for any uint32 chip id.
  String worst = defaultDeviceName("split-flap", 0xFFFFFFFFUL);
  TEST_ASSERT_TRUE((int)worst.length() <= DEVICE_NAME_MAX_LEN);
  TEST_ASSERT_TRUE(isValidDeviceName(worst));
}

// --- resolution order --------------------------------------------------------

static void test_resolve_prefers_valid_stored_name() {
  String got = resolveDeviceName(true, String("kitchen"), "split-flap", 0x9a3c1fUL);
  TEST_ASSERT_EQUAL_STRING("kitchen", got.c_str());
}

static void test_resolve_falls_back_when_slot_empty() {
  String got = resolveDeviceName(true, String(""), "split-flap", 0x9a3c1fUL);
  TEST_ASSERT_EQUAL_STRING("split-flap-9a3c1f", got.c_str());
}

static void test_resolve_falls_back_when_eeprom_invalid() {
  // Bad magic / pre-migration blob: whatever the slot bytes decode to must
  // be ignored -> chip-id default. This is the recovery-mode-before-
  // initialiseSettings() safety property.
  String got = resolveDeviceName(false, String("kitchen"), "split-flap", 0x9a3c1fUL);
  TEST_ASSERT_EQUAL_STRING("split-flap-9a3c1f", got.c_str());
}

static void test_resolve_falls_back_on_garbage_stored_name() {
  String got = resolveDeviceName(true, String("Bad Name!"), "split-flap", 0x9a3c1fUL);
  TEST_ASSERT_EQUAL_STRING("split-flap-9a3c1f", got.c_str());
}

// --- SSID length invariants ---------------------------------------------------

static void test_ap_suffixes_fit_ssid_limit_at_max_name_length() {
  // IEEE 802.11 SSIDs cap at 32 bytes. Every composed AP name must fit
  // even for a maximum-length device name.
  TEST_ASSERT_TRUE(DEVICE_NAME_MAX_LEN + strlen(AP_SUFFIX_SETUP) <= 32);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_normalize_lowercases_input);
  RUN_TEST(test_normalize_leaves_valid_input_untouched);
  RUN_TEST(test_valid_names_accepted);
  RUN_TEST(test_empty_name_is_not_valid);
  RUN_TEST(test_leading_or_trailing_hyphen_rejected);
  RUN_TEST(test_invalid_characters_rejected);
  RUN_TEST(test_length_bounds);
  RUN_TEST(test_default_name_composes_prefix_and_hex_chip_id);
  RUN_TEST(test_default_name_fits_limit_for_max_chip_id);
  RUN_TEST(test_resolve_prefers_valid_stored_name);
  RUN_TEST(test_resolve_falls_back_when_slot_empty);
  RUN_TEST(test_resolve_falls_back_when_eeprom_invalid);
  RUN_TEST(test_resolve_falls_back_on_garbage_stored_name);
  RUN_TEST(test_ap_suffixes_fit_ssid_limit_at_max_name_length);
  return UNITY_END();
}
