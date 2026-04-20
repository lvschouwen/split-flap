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
  TEST_ASSERT_EQUAL_INT(LEN_RESERVED_1, OFF_ALIGNMENT  - OFF_RESERVED_1);
  TEST_ASSERT_EQUAL_INT(LEN_ALIGNMENT,  OFF_FLAPSPEED  - OFF_ALIGNMENT);
  TEST_ASSERT_EQUAL_INT(LEN_FLAPSPEED,  OFF_DEVICEMODE - OFF_FLAPSPEED);
  TEST_ASSERT_EQUAL_INT(LEN_DEVICEMODE, OFF_SCHEDMSGS  - OFF_DEVICEMODE);
}

static void test_layout_fits_in_configured_eeprom_size() {
  int endOfBlob = OFF_SCHEDMSGS + LEN_SCHEDMSGS;
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
  writeSettingString(OFF_ALIGNMENT,  LEN_ALIGNMENT,  String("right"));
  writeSettingString(OFF_FLAPSPEED,  LEN_FLAPSPEED,  String("80"));
  writeSettingString(OFF_DEVICEMODE, LEN_DEVICEMODE, String("clock"));
  writeSettingString(OFF_SCHEDMSGS,  LEN_SCHEDMSGS,
    String("[{\"scheduledDateTimeUnix\":1,\"message\":\"HI\"}]"));

  TEST_ASSERT_EQUAL_STRING("right",      readSettingString(OFF_ALIGNMENT,  LEN_ALIGNMENT).c_str());
  TEST_ASSERT_EQUAL_STRING("80",         readSettingString(OFF_FLAPSPEED,  LEN_FLAPSPEED).c_str());
  TEST_ASSERT_EQUAL_STRING("clock",      readSettingString(OFF_DEVICEMODE, LEN_DEVICEMODE).c_str());
  TEST_ASSERT_EQUAL_STRING(
    "[{\"scheduledDateTimeUnix\":1,\"message\":\"HI\"}]",
    readSettingString(OFF_SCHEDMSGS, LEN_SCHEDMSGS).c_str());
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
  RUN_TEST(test_fresh_eeprom_reads_as_unset_magic);
  RUN_TEST(test_writeSettingMagic_sets_magic_and_version);
  RUN_TEST(test_stale_magic_is_not_accepted);
  RUN_TEST(test_writeSettingString_then_read_roundtrips);
  RUN_TEST(test_writeSettingString_pads_remainder_with_NUL);
  RUN_TEST(test_writeSettingString_truncates_overlong_input);
  RUN_TEST(test_all_slots_roundtrip_independently);
  RUN_TEST(test_readSettingString_stops_at_NUL);
  RUN_TEST(test_writeSettingString_does_not_touch_neighbouring_slots);
  return UNITY_END();
}
