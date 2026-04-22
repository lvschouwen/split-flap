// Host-side tests for SettingsEepromLayout.h.
//
// Exercises the EEPROM blob read/write helpers against the ArduinoFake
// EEPROM mock (virtual methods on EEPROMClass, redirected through fakeit
// to a byte buffer). Verifies layout invariants, round-trip of each
// slot, magic/version handling, and truncation behaviour.

#include <ArduinoFake.h>
#include <unity.h>

#include <array>
#include <cstring>

#include "../../SettingsEepromLayout.h"

using namespace fakeit;

// Backing buffer for the mocked EEPROM. Sized to match the ESP8266 core's
// 4 KB reserved region so out-of-range writes would trip asserts rather
// than corrupt unrelated memory.
static std::array<uint8_t, 4096> g_eepromStorage;

static void wireEepromMock(uint8_t initial) {
  g_eepromStorage.fill(initial);
  When(Method(ArduinoFake(EEPROM), read)).AlwaysDo([](int idx) -> uint8_t {
    if (idx < 0 || (size_t)idx >= g_eepromStorage.size()) return 0xFF;
    return g_eepromStorage[(size_t)idx];
  });
  When(Method(ArduinoFake(EEPROM), write)).AlwaysDo([](int idx, uint8_t val) {
    if (idx < 0 || (size_t)idx >= g_eepromStorage.size()) return;
    g_eepromStorage[(size_t)idx] = val;
  });
}

void setUp() {
  ArduinoFakeReset();
  // Default every test to a blank-flash buffer (0xFF).
  wireEepromMock(0xFF);
}
void tearDown() {}

// --- layout invariants ---------------------------------------------------

static void test_layout_slots_are_contiguous_and_non_overlapping() {
  TEST_ASSERT_EQUAL_INT(4, OFF_VERSION - OFF_MAGIC);
  TEST_ASSERT_EQUAL_INT(1, OFF_RESERVED_1 - OFF_VERSION);
  TEST_ASSERT_EQUAL_INT(LEN_RESERVED_1,        OFF_ALIGNMENT          - OFF_RESERVED_1);
  TEST_ASSERT_EQUAL_INT(LEN_ALIGNMENT,         OFF_FLAPSPEED          - OFF_ALIGNMENT);
  TEST_ASSERT_EQUAL_INT(LEN_FLAPSPEED,         OFF_DEVICEMODE         - OFF_FLAPSPEED);
  TEST_ASSERT_EQUAL_INT(LEN_DEVICEMODE,        OFF_TIMEZONE           - OFF_DEVICEMODE);
  TEST_ASSERT_EQUAL_INT(LEN_TIMEZONE,          OFF_INTENDED_VERSION   - OFF_TIMEZONE);
  TEST_ASSERT_EQUAL_INT(LEN_INTENDED_VERSION,  OFF_LAST_FLASH_RESULT  - OFF_INTENDED_VERSION);
  TEST_ASSERT_EQUAL_INT(LEN_LAST_FLASH_RESULT, OFF_RESERVED_2         - OFF_LAST_FLASH_RESULT);
}

static void test_settings_version_is_4() {
  // Locks the current schema version so a migration addition without a
  // version bump trips here.
  TEST_ASSERT_EQUAL_INT(4, SETTINGS_VERSION);
}

static void test_last_flash_result_slot_fits_known_values() {
  // "reverted" is the longest value we emit (8 chars + NUL = 9). Leave
  // headroom for future labels like "upload-aborted" (15+NUL).
  TEST_ASSERT_GREATER_OR_EQUAL_INT(16, LEN_LAST_FLASH_RESULT);
}

static void test_intended_version_slot_fits_git_rev_plus_dirty_suffix() {
  // Longest realistic rev string: "<8-hex>-dirty" = 14 chars + NUL = 15.
  // 24 bytes gives comfortable headroom for longer tags (release names etc.)
  // while still leaving 0 padding.
  TEST_ASSERT_GREATER_OR_EQUAL_INT(15, LEN_INTENDED_VERSION);
}

static void test_layout_fits_in_configured_eeprom_size() {
  int endOfBlob = OFF_RESERVED_2 + LEN_RESERVED_2;
  TEST_ASSERT_LESS_OR_EQUAL_INT(SETTINGS_EEPROM_SIZE, endOfBlob);
}

// --- magic / version -----------------------------------------------------

static void test_fresh_eeprom_reads_as_unset_magic() {
  // Buffer is 0xFF, so the 32-bit magic reads as 0xFFFFFFFF (!= SETTINGS_MAGIC).
  TEST_ASSERT_NOT_EQUAL_UINT32((uint32_t)SETTINGS_MAGIC, readSettingMagic());
}

static void test_writeSettingMagic_sets_magic_and_version() {
  writeSettingMagic();
  TEST_ASSERT_EQUAL_UINT32((uint32_t)SETTINGS_MAGIC, readSettingMagic());
  TEST_ASSERT_EQUAL_UINT8(SETTINGS_VERSION, g_eepromStorage[OFF_VERSION]);
}

static void test_stale_magic_is_not_accepted() {
  // Write a different 32-bit marker. Mimics a previous firmware's blob.
  uint32_t stale = 0xDEADBEEFUL;
  for (int i = 0; i < 4; i++) {
    g_eepromStorage[OFF_MAGIC + i] = (uint8_t)((stale >> (i * 8)) & 0xFF);
  }
  TEST_ASSERT_NOT_EQUAL_UINT32((uint32_t)SETTINGS_MAGIC, readSettingMagic());
}

// --- read / write round-trip --------------------------------------------

static void test_writeSettingString_then_read_roundtrips() {
  writeSettingString(OFF_ALIGNMENT, LEN_ALIGNMENT, String("center"));
  String got = readSettingString(OFF_ALIGNMENT, LEN_ALIGNMENT);
  TEST_ASSERT_EQUAL_STRING("center", got.c_str());
}

static void test_writeSettingString_pads_remainder_with_NUL() {
  writeSettingString(OFF_DEVICEMODE, LEN_DEVICEMODE, String("text"));
  for (int i = 4; i < LEN_DEVICEMODE; i++) {
    TEST_ASSERT_EQUAL_UINT8(0x00, g_eepromStorage[OFF_DEVICEMODE + i]);
  }
}

static void test_writeSettingString_truncates_overlong_input() {
  // LEN_ALIGNMENT is 8 -> value must be at most 7 chars (room for NUL).
  String tooLong = String("abcdefghij");  // 10 chars
  writeSettingString(OFF_ALIGNMENT, LEN_ALIGNMENT, tooLong);
  String got = readSettingString(OFF_ALIGNMENT, LEN_ALIGNMENT);
  TEST_ASSERT_EQUAL_INT(LEN_ALIGNMENT - 1, (int)got.length());
  TEST_ASSERT_EQUAL_STRING("abcdefg", got.c_str());
  // And the trailing byte must be the NUL terminator.
  TEST_ASSERT_EQUAL_UINT8(0x00, g_eepromStorage[OFF_ALIGNMENT + LEN_ALIGNMENT - 1]);
}

static void test_all_slots_roundtrip_independently() {
  writeSettingString(OFF_ALIGNMENT,        LEN_ALIGNMENT,        String("right"));
  writeSettingString(OFF_FLAPSPEED,        LEN_FLAPSPEED,        String("80"));
  writeSettingString(OFF_DEVICEMODE,       LEN_DEVICEMODE,       String("clock"));
  writeSettingString(OFF_TIMEZONE,         LEN_TIMEZONE,         String("CET-1CEST,M3.5.0,M10.5.0/3"));
  writeSettingString(OFF_INTENDED_VERSION, LEN_INTENDED_VERSION, String("9451352-dirty"));

  TEST_ASSERT_EQUAL_STRING("right",                       readSettingString(OFF_ALIGNMENT,        LEN_ALIGNMENT).c_str());
  TEST_ASSERT_EQUAL_STRING("80",                          readSettingString(OFF_FLAPSPEED,        LEN_FLAPSPEED).c_str());
  TEST_ASSERT_EQUAL_STRING("clock",                       readSettingString(OFF_DEVICEMODE,       LEN_DEVICEMODE).c_str());
  TEST_ASSERT_EQUAL_STRING("CET-1CEST,M3.5.0,M10.5.0/3",  readSettingString(OFF_TIMEZONE,         LEN_TIMEZONE).c_str());
  TEST_ASSERT_EQUAL_STRING("9451352-dirty",               readSettingString(OFF_INTENDED_VERSION, LEN_INTENDED_VERSION).c_str());
}

static void test_intended_version_write_does_not_touch_timezone_or_reserved_2() {
  // Fence-post check that the new slot sits cleanly between OFF_TIMEZONE
  // (end) and OFF_RESERVED_2 (start) without spilling into either.
  g_eepromStorage[OFF_INTENDED_VERSION - 1] = 0xAB;  // last byte of timezone slot
  g_eepromStorage[OFF_INTENDED_VERSION + LEN_INTENDED_VERSION] = 0xCD;  // first byte of reserved_2

  writeSettingString(OFF_INTENDED_VERSION, LEN_INTENDED_VERSION, String("abc"));

  TEST_ASSERT_EQUAL_UINT8(0xAB, g_eepromStorage[OFF_INTENDED_VERSION - 1]);
  TEST_ASSERT_EQUAL_UINT8(0xCD, g_eepromStorage[OFF_INTENDED_VERSION + LEN_INTENDED_VERSION]);
}

static void test_intended_version_empty_after_zero_fill_migration() {
  // v2 -> v3 migration zeros the new slot. With 0x00 at offset 0,
  // readSettingString must return an empty string (not whatever trailing
  // garbage sat in RESERVED_2 before the carve).
  for (int i = 0; i < LEN_INTENDED_VERSION; i++) {
    g_eepromStorage[OFF_INTENDED_VERSION + i] = 0xFF;
  }
  // Apply the migration step as it's written in ServiceFileSystemFunctions.ino.
  writeSettingString(OFF_INTENDED_VERSION, LEN_INTENDED_VERSION, String(""));
  TEST_ASSERT_EQUAL_STRING("", readSettingString(OFF_INTENDED_VERSION, LEN_INTENDED_VERSION).c_str());
}

static void test_last_flash_result_roundtrip() {
  writeSettingString(OFF_LAST_FLASH_RESULT, LEN_LAST_FLASH_RESULT, String("reverted"));
  TEST_ASSERT_EQUAL_STRING("reverted", readSettingString(OFF_LAST_FLASH_RESULT, LEN_LAST_FLASH_RESULT).c_str());

  writeSettingString(OFF_LAST_FLASH_RESULT, LEN_LAST_FLASH_RESULT, String("ok"));
  TEST_ASSERT_EQUAL_STRING("ok", readSettingString(OFF_LAST_FLASH_RESULT, LEN_LAST_FLASH_RESULT).c_str());
}

static void test_last_flash_result_write_does_not_touch_intended_version_or_reserved_2() {
  // Fence-post: writing the new slot must not spill into the intended-version
  // slot (ends at OFF_LAST_FLASH_RESULT-1) or the RESERVED_2 region.
  g_eepromStorage[OFF_LAST_FLASH_RESULT - 1]                         = 0xAB;
  g_eepromStorage[OFF_LAST_FLASH_RESULT + LEN_LAST_FLASH_RESULT]     = 0xCD;

  writeSettingString(OFF_LAST_FLASH_RESULT, LEN_LAST_FLASH_RESULT, String("ok"));

  TEST_ASSERT_EQUAL_UINT8(0xAB, g_eepromStorage[OFF_LAST_FLASH_RESULT - 1]);
  TEST_ASSERT_EQUAL_UINT8(0xCD, g_eepromStorage[OFF_LAST_FLASH_RESULT + LEN_LAST_FLASH_RESULT]);
}

static void test_last_flash_result_empty_after_zero_fill_migration() {
  // v3 -> v4 migration zeros the new slot. Same check pattern as the
  // v2 -> v3 intended-version migration above.
  for (int i = 0; i < LEN_LAST_FLASH_RESULT; i++) {
    g_eepromStorage[OFF_LAST_FLASH_RESULT + i] = 0xFF;
  }
  writeSettingString(OFF_LAST_FLASH_RESULT, LEN_LAST_FLASH_RESULT, String(""));
  TEST_ASSERT_EQUAL_STRING("", readSettingString(OFF_LAST_FLASH_RESULT, LEN_LAST_FLASH_RESULT).c_str());
}

// --- migration -----------------------------------------------------------

static void test_timezone_slot_fits_longest_common_posix_tz() {
  // Check the longest POSIX TZ string we plan to ship in the dropdown.
  // Sydney is the longest at ~28 chars: "AEST-10AEDT,M10.1.0,M4.1.0/3"
  const char* sydney = "AEST-10AEDT,M10.1.0,M4.1.0/3";
  TEST_ASSERT_TRUE(strlen(sydney) + 1 <= LEN_TIMEZONE);
}

static void test_readSettingString_stops_at_NUL() {
  // Write 'Hi' + NUL + junk bytes; read must stop at the NUL.
  g_eepromStorage[OFF_ALIGNMENT + 0] = 'H';
  g_eepromStorage[OFF_ALIGNMENT + 1] = 'i';
  g_eepromStorage[OFF_ALIGNMENT + 2] = 0;
  g_eepromStorage[OFF_ALIGNMENT + 3] = 'X';
  g_eepromStorage[OFF_ALIGNMENT + 4] = 'Y';

  String got = readSettingString(OFF_ALIGNMENT, LEN_ALIGNMENT);
  TEST_ASSERT_EQUAL_STRING("Hi", got.c_str());
}

static void test_writeSettingString_does_not_touch_neighbouring_slots() {
  g_eepromStorage[OFF_ALIGNMENT - 1]               = 0xAB;
  g_eepromStorage[OFF_ALIGNMENT + LEN_ALIGNMENT]   = 0xCD;

  writeSettingString(OFF_ALIGNMENT, LEN_ALIGNMENT, String("left"));

  TEST_ASSERT_EQUAL_UINT8(0xAB, g_eepromStorage[OFF_ALIGNMENT - 1]);
  TEST_ASSERT_EQUAL_UINT8(0xCD, g_eepromStorage[OFF_ALIGNMENT + LEN_ALIGNMENT]);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_layout_slots_are_contiguous_and_non_overlapping);
  RUN_TEST(test_layout_fits_in_configured_eeprom_size);
  RUN_TEST(test_settings_version_is_4);
  RUN_TEST(test_intended_version_slot_fits_git_rev_plus_dirty_suffix);
  RUN_TEST(test_last_flash_result_slot_fits_known_values);
  RUN_TEST(test_fresh_eeprom_reads_as_unset_magic);
  RUN_TEST(test_writeSettingMagic_sets_magic_and_version);
  RUN_TEST(test_stale_magic_is_not_accepted);
  RUN_TEST(test_writeSettingString_then_read_roundtrips);
  RUN_TEST(test_writeSettingString_pads_remainder_with_NUL);
  RUN_TEST(test_writeSettingString_truncates_overlong_input);
  RUN_TEST(test_all_slots_roundtrip_independently);
  RUN_TEST(test_intended_version_write_does_not_touch_timezone_or_reserved_2);
  RUN_TEST(test_intended_version_empty_after_zero_fill_migration);
  RUN_TEST(test_last_flash_result_roundtrip);
  RUN_TEST(test_last_flash_result_write_does_not_touch_intended_version_or_reserved_2);
  RUN_TEST(test_last_flash_result_empty_after_zero_fill_migration);
  RUN_TEST(test_timezone_slot_fits_longest_common_posix_tz);
  RUN_TEST(test_readSettingString_stops_at_NUL);
  RUN_TEST(test_writeSettingString_does_not_touch_neighbouring_slots);
  return UNITY_END();
}
