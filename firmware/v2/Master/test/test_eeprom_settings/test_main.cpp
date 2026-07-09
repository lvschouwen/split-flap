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
  TEST_ASSERT_EQUAL_INT(1, OFF_SETTINGS_CRC - OFF_VERSION);
  TEST_ASSERT_EQUAL_INT(LEN_SETTINGS_CRC,     OFF_RESERVED_1         - OFF_SETTINGS_CRC);
  TEST_ASSERT_EQUAL_INT(LEN_RESERVED_1,        OFF_ALIGNMENT          - OFF_RESERVED_1);
  TEST_ASSERT_EQUAL_INT(LEN_ALIGNMENT,         OFF_FLAPSPEED          - OFF_ALIGNMENT);
  TEST_ASSERT_EQUAL_INT(LEN_FLAPSPEED,         OFF_DEVICEMODE         - OFF_FLAPSPEED);
  TEST_ASSERT_EQUAL_INT(LEN_DEVICEMODE,        OFF_TIMEZONE           - OFF_DEVICEMODE);
  TEST_ASSERT_EQUAL_INT(LEN_TIMEZONE,          OFF_INTENDED_VERSION   - OFF_TIMEZONE);
  TEST_ASSERT_EQUAL_INT(LEN_INTENDED_VERSION,  OFF_LAST_FLASH_RESULT  - OFF_INTENDED_VERSION);
  TEST_ASSERT_EQUAL_INT(LEN_LAST_FLASH_RESULT, OFF_DEVICE_NAME        - OFF_LAST_FLASH_RESULT);
  TEST_ASSERT_EQUAL_INT(LEN_DEVICE_NAME,       OFF_MQTT_HOST          - OFF_DEVICE_NAME);
  TEST_ASSERT_EQUAL_INT(LEN_MQTT_HOST,         OFF_MQTT_PORT          - OFF_MQTT_HOST);
  TEST_ASSERT_EQUAL_INT(LEN_MQTT_PORT,         OFF_MQTT_USER          - OFF_MQTT_PORT);
  TEST_ASSERT_EQUAL_INT(LEN_MQTT_USER,         OFF_MQTT_PASSWORD      - OFF_MQTT_USER);
  TEST_ASSERT_EQUAL_INT(LEN_MQTT_PASSWORD,     OFF_RESERVED_2         - OFF_MQTT_PASSWORD);
}

static void test_settings_version_is_7() {
  // Locks the current schema version so a migration addition without a
  // version bump trips here.
  TEST_ASSERT_EQUAL_INT(7, SETTINGS_VERSION);
}

static void test_crc_slot_carved_from_reserved_1() {
  // #151: the CRC32 slot is carved in place from the RESERVED_1 fossil at
  // offset 5, so every pre-existing slot keeps its offset (OFF_ALIGNMENT
  // must stay at 25 — the whole layout after RESERVED_1 is frozen).
  TEST_ASSERT_EQUAL_INT(OFF_VERSION + 1, OFF_SETTINGS_CRC);
  TEST_ASSERT_EQUAL_INT(4, LEN_SETTINGS_CRC);
  TEST_ASSERT_EQUAL_INT(OFF_SETTINGS_CRC + LEN_SETTINGS_CRC, OFF_RESERVED_1);
  TEST_ASSERT_EQUAL_INT(25, OFF_RESERVED_1 + LEN_RESERVED_1);
  TEST_ASSERT_EQUAL_INT(25, OFF_ALIGNMENT);
}

static void test_device_name_slot_fits_max_name_plus_nul() {
  // #125: device names are capped at 24 chars; slot needs 24 + NUL.
  TEST_ASSERT_GREATER_OR_EQUAL_INT(25, LEN_DEVICE_NAME);
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
  // Apply the migration step as it's written in ServiceSettingsFunctions.ino.
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

static void test_device_name_roundtrip() {
  writeSettingString(OFF_DEVICE_NAME, LEN_DEVICE_NAME, String("split-flap-livingroom"));
  TEST_ASSERT_EQUAL_STRING("split-flap-livingroom", readSettingString(OFF_DEVICE_NAME, LEN_DEVICE_NAME).c_str());
}

static void test_device_name_write_does_not_touch_last_flash_result_or_reserved_2() {
  // Fence-post: writing the new slot must not spill into the last-flash-result
  // slot (ends at OFF_DEVICE_NAME-1) or the RESERVED_2 region.
  g_eepromStorage[OFF_DEVICE_NAME - 1]               = 0xAB;
  g_eepromStorage[OFF_DEVICE_NAME + LEN_DEVICE_NAME] = 0xCD;

  writeSettingString(OFF_DEVICE_NAME, LEN_DEVICE_NAME, String("kitchen"));

  TEST_ASSERT_EQUAL_UINT8(0xAB, g_eepromStorage[OFF_DEVICE_NAME - 1]);
  TEST_ASSERT_EQUAL_UINT8(0xCD, g_eepromStorage[OFF_DEVICE_NAME + LEN_DEVICE_NAME]);
}

static void test_device_name_empty_after_zero_fill_migration() {
  // v4 -> v5 migration zeros the new slot so a migrated device reads an
  // empty name (-> chip-id default), not RESERVED_2 leftovers. See #125.
  for (int i = 0; i < LEN_DEVICE_NAME; i++) {
    g_eepromStorage[OFF_DEVICE_NAME + i] = 0xFF;
  }
  writeSettingString(OFF_DEVICE_NAME, LEN_DEVICE_NAME, String(""));
  TEST_ASSERT_EQUAL_STRING("", readSettingString(OFF_DEVICE_NAME, LEN_DEVICE_NAME).c_str());
}

// --- MQTT broker config slots (#57, v6) -----------------------------------

static void test_mqtt_slots_roundtrip() {
  writeSettingString(OFF_MQTT_HOST,     LEN_MQTT_HOST,     String("homeassistant.local"));
  writeSettingString(OFF_MQTT_PORT,     LEN_MQTT_PORT,     String("1883"));
  writeSettingString(OFF_MQTT_USER,     LEN_MQTT_USER,     String("splitflap"));
  writeSettingString(OFF_MQTT_PASSWORD, LEN_MQTT_PASSWORD, String("s3cret-Pa55!"));

  TEST_ASSERT_EQUAL_STRING("homeassistant.local", readSettingString(OFF_MQTT_HOST,     LEN_MQTT_HOST).c_str());
  TEST_ASSERT_EQUAL_STRING("1883",                readSettingString(OFF_MQTT_PORT,     LEN_MQTT_PORT).c_str());
  TEST_ASSERT_EQUAL_STRING("splitflap",           readSettingString(OFF_MQTT_USER,     LEN_MQTT_USER).c_str());
  TEST_ASSERT_EQUAL_STRING("s3cret-Pa55!",        readSettingString(OFF_MQTT_PASSWORD, LEN_MQTT_PASSWORD).c_str());
}

static void test_mqtt_port_slot_fits_max_port() {
  // "65535" = 5 chars + NUL.
  TEST_ASSERT_GREATER_OR_EQUAL_INT(6, LEN_MQTT_PORT);
}

static void test_mqtt_host_write_does_not_touch_device_name_or_port() {
  // Fence-post: the first new v6 slot must not spill into deviceName
  // (ends at OFF_MQTT_HOST-1) or the port slot after it.
  g_eepromStorage[OFF_MQTT_HOST - 1]             = 0xAB;
  g_eepromStorage[OFF_MQTT_HOST + LEN_MQTT_HOST] = 0xCD;

  writeSettingString(OFF_MQTT_HOST, LEN_MQTT_HOST, String("192.168.1.10"));

  TEST_ASSERT_EQUAL_UINT8(0xAB, g_eepromStorage[OFF_MQTT_HOST - 1]);
  TEST_ASSERT_EQUAL_UINT8(0xCD, g_eepromStorage[OFF_MQTT_HOST + LEN_MQTT_HOST]);
}

static void test_mqtt_password_write_does_not_touch_user_or_reserved_2() {
  // Fence-post: the last new v6 slot must not spill into the RESERVED_2
  // region that follows it.
  g_eepromStorage[OFF_MQTT_PASSWORD - 1]                 = 0xAB;
  g_eepromStorage[OFF_MQTT_PASSWORD + LEN_MQTT_PASSWORD] = 0xCD;

  writeSettingString(OFF_MQTT_PASSWORD, LEN_MQTT_PASSWORD, String("hunter2"));

  TEST_ASSERT_EQUAL_UINT8(0xAB, g_eepromStorage[OFF_MQTT_PASSWORD - 1]);
  TEST_ASSERT_EQUAL_UINT8(0xCD, g_eepromStorage[OFF_MQTT_PASSWORD + LEN_MQTT_PASSWORD]);
}

static void test_mqtt_slots_empty_after_zero_fill_migration() {
  // v5 -> v6 migration zeros all four slots so a migrated device reads an
  // empty host (-> MQTT stays disabled), not RESERVED_2 leftovers. See #57.
  for (int i = 0; i < LEN_MQTT_HOST + LEN_MQTT_PORT + LEN_MQTT_USER + LEN_MQTT_PASSWORD; i++) {
    g_eepromStorage[OFF_MQTT_HOST + i] = 0xFF;
  }
  writeSettingString(OFF_MQTT_HOST,     LEN_MQTT_HOST,     String(""));
  writeSettingString(OFF_MQTT_PORT,     LEN_MQTT_PORT,     String(""));
  writeSettingString(OFF_MQTT_USER,     LEN_MQTT_USER,     String(""));
  writeSettingString(OFF_MQTT_PASSWORD, LEN_MQTT_PASSWORD, String(""));
  TEST_ASSERT_EQUAL_STRING("", readSettingString(OFF_MQTT_HOST,     LEN_MQTT_HOST).c_str());
  TEST_ASSERT_EQUAL_STRING("", readSettingString(OFF_MQTT_PORT,     LEN_MQTT_PORT).c_str());
  TEST_ASSERT_EQUAL_STRING("", readSettingString(OFF_MQTT_USER,     LEN_MQTT_USER).c_str());
  TEST_ASSERT_EQUAL_STRING("", readSettingString(OFF_MQTT_PASSWORD, LEN_MQTT_PASSWORD).c_str());
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

// --- settings CRC (#151) + downgrade preservation (#152) ------------------

static void test_settings_crc_roundtrip_and_fence_posts() {
  g_eepromStorage[OFF_SETTINGS_CRC - 1]                    = 0xAB;  // version byte's slot boundary
  g_eepromStorage[OFF_SETTINGS_CRC + LEN_SETTINGS_CRC]     = 0xCD;  // first byte of RESERVED_1
  writeSettingsCrc(0x12345678UL);
  TEST_ASSERT_EQUAL_UINT32(0x12345678UL, readSettingsCrc());
  TEST_ASSERT_EQUAL_UINT8(0xAB, g_eepromStorage[OFF_SETTINGS_CRC - 1]);
  TEST_ASSERT_EQUAL_UINT8(0xCD, g_eepromStorage[OFF_SETTINGS_CRC + LEN_SETTINGS_CRC]);
}

static void test_computeSettingsCrc_covers_first_and_last_payload_byte() {
  // Coverage is the FULL payload [OFF_ALIGNMENT, end of blob), reserved
  // regions included — that keeps the covered range version-independent,
  // which is what lets older firmware validate a newer blob's CRC on the
  // #152 downgrade path.
  uint32_t base = computeSettingsCrc();

  g_eepromStorage[OFF_ALIGNMENT] ^= 0x01;
  TEST_ASSERT_NOT_EQUAL_UINT32(base, computeSettingsCrc());
  g_eepromStorage[OFF_ALIGNMENT] ^= 0x01;
  TEST_ASSERT_EQUAL_UINT32(base, computeSettingsCrc());

  g_eepromStorage[OFF_RESERVED_2 + LEN_RESERVED_2 - 1] ^= 0x01;
  TEST_ASSERT_NOT_EQUAL_UINT32(base, computeSettingsCrc());
}

static void test_computeSettingsCrc_excludes_header_bytes() {
  // Magic, version and the CRC slot itself must not feed the CRC — the
  // downgrade path rewrites the version byte and expects the stored CRC
  // to stay valid.
  uint32_t base = computeSettingsCrc();
  g_eepromStorage[OFF_MAGIC]        ^= 0xFF;
  g_eepromStorage[OFF_VERSION]      ^= 0xFF;
  g_eepromStorage[OFF_SETTINGS_CRC] ^= 0xFF;
  TEST_ASSERT_EQUAL_UINT32(base, computeSettingsCrc());
}

static void test_updateSettingsCrc_makes_stored_crc_match() {
  writeSettingString(OFF_ALIGNMENT, LEN_ALIGNMENT, String("center"));
  updateSettingsCrc();
  TEST_ASSERT_EQUAL_UINT32(computeSettingsCrc(), readSettingsCrc());
}

static void test_assess_blank_magic_is_blank() {
  TEST_ASSERT_EQUAL_INT(SETTINGS_BLOB_BLANK,
      assessSettingsBlob(0xFFFFFFFFUL, 0xFF, 0, 0));
}

static void test_assess_current_version_valid_crc_is_ok() {
  TEST_ASSERT_EQUAL_INT(SETTINGS_BLOB_OK,
      assessSettingsBlob(SETTINGS_MAGIC, SETTINGS_VERSION, 0xC0FFEEUL, 0xC0FFEEUL));
}

static void test_assess_current_version_bad_crc_is_corrupt() {
  // #151: torn commit — magic/version intact, payload garbage.
  TEST_ASSERT_EQUAL_INT(SETTINGS_BLOB_CORRUPT,
      assessSettingsBlob(SETTINGS_MAGIC, SETTINGS_VERSION, 0xC0FFEEUL, 0xBADBADUL));
}

static void test_assess_older_version_bad_crc_is_migrate() {
  // Pre-v7 blobs have no CRC — the slot holds RESERVED_1 garbage that
  // (all but certainly) doesn't match, and must not block the ladder.
  TEST_ASSERT_EQUAL_INT(SETTINGS_BLOB_MIGRATE,
      assessSettingsBlob(SETTINGS_MAGIC, SETTINGS_VERSION - 1, 0x11UL, 0x22UL));
  TEST_ASSERT_EQUAL_INT(SETTINGS_BLOB_MIGRATE,
      assessSettingsBlob(SETTINGS_MAGIC, 1, 0x11UL, 0x22UL));
}

static void test_assess_older_version_valid_crc_is_adopted_not_migrated() {
  // A VALID CRC on an "older-version" blob is a 2^-32 coincidence for a
  // genuine pre-v7 blob — in practice it means a current blob whose version
  // byte alone bit-rotted low. Running the migration ladder would zero
  // perfectly good fields; adopt the payload and pin the version instead.
  TEST_ASSERT_EQUAL_INT(SETTINGS_BLOB_ADOPT,
      assessSettingsBlob(SETTINGS_MAGIC, SETTINGS_VERSION - 1, 0xC0FFEEUL, 0xC0FFEEUL));
}

static void test_assess_newer_version_valid_crc_is_adopted() {
  // #152: OTA revert to older firmware — blob intact, just newer. Must be
  // preserved, not wiped.
  TEST_ASSERT_EQUAL_INT(SETTINGS_BLOB_ADOPT,
      assessSettingsBlob(SETTINGS_MAGIC, SETTINGS_VERSION + 1, 0xC0FFEEUL, 0xC0FFEEUL));
}

static void test_assess_newer_version_bad_crc_is_corrupt() {
  // Torn header write can leave version = 0xFF with garbage payload; that
  // must NOT be mistaken for a genuine downgrade.
  TEST_ASSERT_EQUAL_INT(SETTINGS_BLOB_CORRUPT,
      assessSettingsBlob(SETTINGS_MAGIC, 0xFF, 0xC0FFEEUL, 0xBADBADUL));
}

static void test_torn_write_scenario_detected_end_to_end() {
  // Build a fully valid blob, then corrupt a single payload byte (power
  // sag mid-commit). The read-back verdict must be CORRUPT.
  writeSettingString(OFF_ALIGNMENT,  LEN_ALIGNMENT,  String("center"));
  writeSettingString(OFF_DEVICE_NAME, LEN_DEVICE_NAME, String("hallway"));
  updateSettingsCrc();
  writeSettingMagic();

  g_eepromStorage[OFF_DEVICE_NAME] ^= 0x40;

  TEST_ASSERT_EQUAL_INT(SETTINGS_BLOB_CORRUPT,
      assessSettingsBlob(readSettingMagic(), g_eepromStorage[OFF_VERSION],
                         readSettingsCrc(), computeSettingsCrc()));
}

static void test_downgrade_scenario_preserves_payload_end_to_end() {
  // Simulate a blob written by a hypothetical v8 firmware (same CRC
  // scheme, same frozen offsets): settings present, version newer.
  writeSettingString(OFF_DEVICE_NAME, LEN_DEVICE_NAME, String("hallway"));
  writeSettingString(OFF_MQTT_HOST,   LEN_MQTT_HOST,   String("192.168.1.10"));
  updateSettingsCrc();
  writeSettingMagic();
  g_eepromStorage[OFF_VERSION] = SETTINGS_VERSION + 1;

  TEST_ASSERT_EQUAL_INT(SETTINGS_BLOB_ADOPT,
      assessSettingsBlob(readSettingMagic(), g_eepromStorage[OFF_VERSION],
                         readSettingsCrc(), computeSettingsCrc()));
  // And the slots the older firmware understands are still readable.
  TEST_ASSERT_EQUAL_STRING("hallway",      readSettingString(OFF_DEVICE_NAME, LEN_DEVICE_NAME).c_str());
  TEST_ASSERT_EQUAL_STRING("192.168.1.10", readSettingString(OFF_MQTT_HOST,   LEN_MQTT_HOST).c_str());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_layout_slots_are_contiguous_and_non_overlapping);
  RUN_TEST(test_layout_fits_in_configured_eeprom_size);
  RUN_TEST(test_settings_version_is_7);
  RUN_TEST(test_crc_slot_carved_from_reserved_1);
  RUN_TEST(test_settings_crc_roundtrip_and_fence_posts);
  RUN_TEST(test_computeSettingsCrc_covers_first_and_last_payload_byte);
  RUN_TEST(test_computeSettingsCrc_excludes_header_bytes);
  RUN_TEST(test_updateSettingsCrc_makes_stored_crc_match);
  RUN_TEST(test_assess_blank_magic_is_blank);
  RUN_TEST(test_assess_current_version_valid_crc_is_ok);
  RUN_TEST(test_assess_current_version_bad_crc_is_corrupt);
  RUN_TEST(test_assess_older_version_bad_crc_is_migrate);
  RUN_TEST(test_assess_older_version_valid_crc_is_adopted_not_migrated);
  RUN_TEST(test_assess_newer_version_valid_crc_is_adopted);
  RUN_TEST(test_assess_newer_version_bad_crc_is_corrupt);
  RUN_TEST(test_torn_write_scenario_detected_end_to_end);
  RUN_TEST(test_downgrade_scenario_preserves_payload_end_to_end);
  RUN_TEST(test_intended_version_slot_fits_git_rev_plus_dirty_suffix);
  RUN_TEST(test_last_flash_result_slot_fits_known_values);
  RUN_TEST(test_device_name_slot_fits_max_name_plus_nul);
  RUN_TEST(test_device_name_roundtrip);
  RUN_TEST(test_device_name_write_does_not_touch_last_flash_result_or_reserved_2);
  RUN_TEST(test_device_name_empty_after_zero_fill_migration);
  RUN_TEST(test_mqtt_slots_roundtrip);
  RUN_TEST(test_mqtt_port_slot_fits_max_port);
  RUN_TEST(test_mqtt_host_write_does_not_touch_device_name_or_port);
  RUN_TEST(test_mqtt_password_write_does_not_touch_user_or_reserved_2);
  RUN_TEST(test_mqtt_slots_empty_after_zero_fill_migration);
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
