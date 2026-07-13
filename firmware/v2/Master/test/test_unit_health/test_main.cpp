// Host-side tests for UnitHealth.h (#203) — v2 copy of v1's pure unit-health
// logic (per the copy policy: v2 pure headers are natively-tested copies).
// Covers the faulty predicate, the faulty count over UnitFacts, the health
// JSON builder (same wire shape as v1's /units/health), the CMD_GET_LETTER
// readback validation, and the twiboot chipinfo signature check.

#include <ArduinoFake.h>
#include <unity.h>

#include <cstring>

#include "../../TwibootProtocol.h"
#include "../../UnitHealth.h"
#include "../../UnitProtocolHelpers.h"
#include "../../WearPolicy.h"
#include "SplitFlapProtocol.h"

void setUp() {}
void tearDown() {}

// --- unitStatusIsFaulty ------------------------------------------------------

static void test_clean_status_is_not_faulty() {
  UnitStatus s{};
  TEST_ASSERT_FALSE(unitStatusIsFaulty(s));
}

static void test_home_failed_flag_is_faulty() {
  UnitStatus s{};
  s.flags = UNIT_FLAG_LAST_HOME_FAILED;
  TEST_ASSERT_TRUE(unitStatusIsFaulty(s));
}

static void test_hall_never_flag_is_faulty() {
  UnitStatus s{};
  s.flags = UNIT_FLAG_HALL_NEVER;
  TEST_ASSERT_TRUE(unitStatusIsFaulty(s));
}

static void test_brownout_or_watchdog_is_faulty() {
  UnitStatus s{};
  s.lifetimeBrownoutCount = 1;
  TEST_ASSERT_TRUE(unitStatusIsFaulty(s));
  UnitStatus w{};
  w.lifetimeWatchdogCount = 3;
  TEST_ASSERT_TRUE(unitStatusIsFaulty(w));
}

static void test_bad_commands_alone_are_not_faulty() {
  // Deliberate (#45/#137): a stray malformed I2C receive is not a hardware
  // problem; badCommandCount is surfaced but never counted.
  UnitStatus s{};
  s.badCommandCount = 9;
  TEST_ASSERT_FALSE(unitStatusIsFaulty(s));
}

static void test_addr_eeprom_flag_alone_is_not_faulty() {
  // Address source (#215) is informational — an EEPROM-provisioned address is
  // a provisioning fact, not a hardware fault.
  UnitStatus s{};
  s.flags = UNIT_FLAG_ADDR_EEPROM;
  TEST_ASSERT_FALSE(unitStatusIsFaulty(s));
}

// --- computeFaultyUnitCount --------------------------------------------------

static void test_faulty_count_only_counts_valid_slots() {
  UnitFacts units[3];
  units[0].statusValid = true;   // valid + faulty -> counts
  units[0].status.lifetimeWatchdogCount = 1;
  units[1].statusValid = false;  // faulty-looking but never read -> ignored
  units[1].status.lifetimeWatchdogCount = 1;
  units[2].statusValid = true;   // valid + clean -> not counted
  TEST_ASSERT_EQUAL(1, computeFaultyUnitCount(units, 3));
}

// --- buildUnitHealthJson -----------------------------------------------------

static void test_health_json_valid_and_bootloader_slots() {
  UnitFacts units[2];
  units[0].state = 1;
  units[0].statusValid = true;
  units[0].fwStatus = 2;
  strcpy(units[0].version, "abc12345");
  units[0].status.flags = 0;
  units[0].status.mcusrAtBoot = 54;
  units[0].status.uptimeSeconds = 1200;
  units[0].status.badCommandCount = 0;
  units[0].status.lastHomingStepCount = 720;
  units[1].state = 2;  // bootloader: bare slot, no health fields

  char buf[512];
  size_t n = buildUnitHealthJson(buf, sizeof(buf), units, 2, 0,
                                 SFP_I2C_ADDRESS_BASE);
  TEST_ASSERT_TRUE(n > 0 && n < sizeof(buf));
  TEST_ASSERT_EQUAL_STRING(
      "{\"width\":2,\"faulty\":0,\"units\":["
      "{\"i\":0,\"a\":1,\"st\":1,\"v\":1,\"fw\":2,\"rev\":\"abc12345\","
      "\"up\":1200,\"br\":0,\"wd\":0,\"bc\":0,\"mc\":54,\"fl\":0,\"hs\":720,\"ae\":0},"
      "{\"i\":1,\"a\":2,\"st\":2,\"v\":0}]}",
      buf);
}

static void test_health_json_addr_eeprom_bit_surfaces_as_ae() {
  // #215: flags bit 4 (address source = EEPROM) is derived into "ae" so the
  // UI never has to know the bitfield layout. The raw bit stays in "fl" too.
  UnitFacts units[1];
  units[0].state = 1;
  units[0].statusValid = true;
  units[0].status.flags = UNIT_FLAG_ADDR_EEPROM;
  char buf[512];
  size_t n = buildUnitHealthJson(buf, sizeof(buf), units, 1, 0, 1);
  TEST_ASSERT_TRUE(n > 0 && n < sizeof(buf));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"fl\":16"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"ae\":1"));
}

static void test_health_json_odometer_emitted_when_valid() {
  // #231: "odo" rides its own valid flag, independent of statusValid —
  // a unit can report status but run pre-odometer firmware.
  UnitFacts units[2];
  units[0].state = 1;
  units[0].statusValid = true;
  units[0].odometer = 123456;
  units[0].odometerValid = true;
  units[1].state = 1;
  units[1].statusValid = true;  // no odometer -> no "odo" key
  char buf[512];
  size_t n = buildUnitHealthJson(buf, sizeof(buf), units, 2, 0, 1);
  TEST_ASSERT_TRUE(n > 0 && n < sizeof(buf));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"odo\":123456"));
  char* second = strstr(buf, "\"i\":1");
  TEST_ASSERT_NOT_NULL(second);
  TEST_ASSERT_NULL(strstr(second, "\"odo\""));
}

// --- odometer readback (SFP_CMD_GET_ODOMETER, #231) --------------------------

static void test_odometer_readback_valid_roundtrip() {
  uint8_t buf[5] = {0x01, 0x02, 0x03, 0x04,
                    (uint8_t)((0x01 ^ 0x02 ^ 0x03 ^ 0x04) ^ 0xA5)};
  uint32_t out = 0;
  TEST_ASSERT_TRUE(odometerReadbackValid(buf, out));
  TEST_ASSERT_EQUAL_UINT32(0x04030201UL, out);
}

static void test_odometer_readback_rejects_old_firmware_garbage() {
  // Pre-odometer firmware answers the unknown opcode with its 1-byte status
  // reply + bus padding: all-0xFF, all-0x00 and repeated 0x01 must all fail.
  uint32_t out;
  uint8_t ff[5] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  uint8_t zero[5] = {0x00, 0x00, 0x00, 0x00, 0x00};
  uint8_t busy[5] = {0x01, 0x01, 0x01, 0x01, 0x01};
  TEST_ASSERT_FALSE(odometerReadbackValid(ff, out));
  TEST_ASSERT_FALSE(odometerReadbackValid(zero, out));
  TEST_ASSERT_FALSE(odometerReadbackValid(busy, out));
}

static void test_health_json_worst_case_fits_cap_with_reflash_headroom() {
  // The endpoint splices a ~70 B reflash progress object (#205) into the
  // same cap-sized buffer — a fully saturated 16-unit payload must leave at
  // least that much headroom or the endpoint degrades to headline-only.
  UnitFacts units[16];
  for (int i = 0; i < 16; i++) {
    units[i].state = 1;
    units[i].statusValid = true;
    units[i].fwStatus = 2;
    strcpy(units[i].version, "abc12345");
    units[i].status.flags = 0xFF;
    units[i].status.mcusrAtBoot = 255;
    units[i].status.lifetimeBrownoutCount = 255;
    units[i].status.lifetimeWatchdogCount = 255;
    units[i].status.uptimeSeconds = 65535;
    units[i].status.badCommandCount = 255;
    units[i].status.lastHomingStepCount = 65520;
    units[i].odometer = 0xFFFFFFFEUL;  // widest possible "odo" field (#231)
    units[i].odometerValid = true;
  }
  char buf[UNIT_HEALTH_JSON_CAP];
  size_t n = buildUnitHealthJson(buf, sizeof(buf), units, 16, 16, 1);
  TEST_ASSERT_TRUE(n < sizeof(buf));
  TEST_ASSERT_TRUE(n + 96 <= UNIT_HEALTH_JSON_CAP);
}

static void test_health_json_combined_splices_fit_cap() {
  // WebEndpoints splices BOTH the wear object (#231) and the reflash object
  // (#205) into the same cap-sized buffer, each before the closing brace.
  // Rebuild that combination at its worst case with the endpoint's exact
  // arithmetic: all three keys must survive, or a future growth of any one
  // piece silently drops the later splice.
  UnitFacts units[16];
  for (int i = 0; i < 16; i++) {
    units[i].state = 1;
    units[i].statusValid = true;
    units[i].fwStatus = 2;
    strcpy(units[i].version, "abc12345");
    units[i].status.flags = 0xFF;
    units[i].status.mcusrAtBoot = 255;
    units[i].status.lifetimeBrownoutCount = 255;
    units[i].status.lifetimeWatchdogCount = 255;
    units[i].status.uptimeSeconds = 65535;
    units[i].status.badCommandCount = 255;
    units[i].status.lastHomingStepCount = 65520;
    units[i].odometer = 0xFFFFFFFEUL;
    units[i].odometerValid = true;
  }
  char buf[UNIT_HEALTH_JSON_CAP];
  size_t n = buildUnitHealthJson(buf, sizeof(buf), units, 16, 16, 1);
  TEST_ASSERT_TRUE(n > 0 && n < sizeof(buf));

  // Worst wear fragment: 10-digit median, every unit flagged (hand-filled —
  // assessWear can never flag all 16, but the buffer must survive it).
  WearAssessment w;
  w.median = 0xFFFFFFFFUL;
  for (int i = 0; i < 16; i++) w.flagged[i] = true;
  w.flaggedCount = 16;
  char wearJson[96];
  size_t wearLen = buildWearJson(w, wearJson, sizeof(wearJson));
  TEST_ASSERT_TRUE(wearLen > 0 && wearLen < sizeof(wearJson));
  TEST_ASSERT_TRUE(n + wearLen + 2 < UNIT_HEALTH_JSON_CAP);
  n += (size_t)snprintf(buf + n - 1, UNIT_HEALTH_JSON_CAP - n + 1, ",%s}",
                        wearJson) - 1;

  // Worst reflash fragment (buildReflashJson's format, saturated fields).
  const char* reflashJson =
      "{\"state\":\"flashing\",\"total\":16,\"done\":16,\"failed\":16,\"cur\":127}";
  TEST_ASSERT_TRUE(n + strlen(reflashJson) + 13 < UNIT_HEALTH_JSON_CAP);
  snprintf(buf + n - 1, UNIT_HEALTH_JSON_CAP - n + 1, ",\"reflash\":%s}",
           reflashJson);

  TEST_ASSERT_NOT_NULL(strstr(buf, "\"odo\":"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"wear\":{"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"reflash\":{"));
  TEST_ASSERT_EQUAL_CHAR('}', buf[strlen(buf) - 1]);
  TEST_ASSERT_TRUE(strlen(buf) < UNIT_HEALTH_JSON_CAP);
}

static void test_health_json_silent_gap_slot() {
  UnitFacts units[1];  // default: state 0, statusValid false
  char buf[128];
  size_t n = buildUnitHealthJson(buf, sizeof(buf), units, 1, 0, 1);
  TEST_ASSERT_TRUE(n > 0 && n < sizeof(buf));
  TEST_ASSERT_EQUAL_STRING(
      "{\"width\":1,\"faulty\":0,\"units\":[{\"i\":0,\"a\":1,\"st\":0,\"v\":0}]}",
      buf);
}

static void test_health_json_overflow_reports_full() {
  UnitFacts units[2];
  units[0].state = 1;
  units[0].statusValid = true;
  char buf[24];
  size_t n = buildUnitHealthJson(buf, sizeof(buf), units, 2, 0, 1);
  // Caller contract (v1 parity): n >= cap means "would truncate", the
  // endpoint falls back to a headline-only JSON.
  TEST_ASSERT_TRUE(n >= sizeof(buf));
}

// --- letterReadbackValid -----------------------------------------------------

static void test_letter_readback_valid_pair() {
  TEST_ASSERT_TRUE(letterReadbackValid(7, (uint8_t)~7, 45));
}

static void test_letter_readback_rejects_bad_complement() {
  TEST_ASSERT_FALSE(letterReadbackValid(7, 7, 45));
}

static void test_letter_readback_rejects_out_of_range() {
  TEST_ASSERT_FALSE(letterReadbackValid(45, (uint8_t)~45, 45));
}

// --- isAtmega328pSignature ---------------------------------------------------

static void test_atmega328p_signature() {
  TEST_ASSERT_TRUE(isAtmega328pSignature(0x1E, 0x95, 0x0F));
  TEST_ASSERT_FALSE(isAtmega328pSignature(0x1E, 0x95, 0x14));  // 328PB
  TEST_ASSERT_FALSE(isAtmega328pSignature(0xFF, 0xFF, 0xFF));  // bus noise
}

// --- fw status vs the bundled rev (#205) ---------------------------------------

static void test_fw_status_matches_bundled_rev() {
  TEST_ASSERT_EQUAL_UINT8(0, unitFwStatusFromRev("26518a1", "26518a1"));
}

static void test_fw_status_outdated_on_mismatch() {
  TEST_ASSERT_EQUAL_UINT8(1, unitFwStatusFromRev("0fd341f", "26518a1"));
}

static void test_fw_status_unknown_on_empty_version_or_bundle() {
  TEST_ASSERT_EQUAL_UINT8(2, unitFwStatusFromRev("", "26518a1"));
  TEST_ASSERT_EQUAL_UINT8(2, unitFwStatusFromRev("26518a1", ""));
}

static void test_fw_status_compares_first_eight_chars_like_v1() {
  // v1 stored 8 chars + NUL and compared with strncmp(..., 8); a longer
  // sidecar rev (e.g. "26518a1e-dirty") must still match its 8-char prefix.
  TEST_ASSERT_EQUAL_UINT8(0, unitFwStatusFromRev("26518a1e", "26518a1e-dirty"));
  TEST_ASSERT_EQUAL_UINT8(1, unitFwStatusFromRev("26518a1e", "26518a1f-dirty"));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_clean_status_is_not_faulty);
  RUN_TEST(test_home_failed_flag_is_faulty);
  RUN_TEST(test_hall_never_flag_is_faulty);
  RUN_TEST(test_brownout_or_watchdog_is_faulty);
  RUN_TEST(test_bad_commands_alone_are_not_faulty);
  RUN_TEST(test_addr_eeprom_flag_alone_is_not_faulty);
  RUN_TEST(test_faulty_count_only_counts_valid_slots);
  RUN_TEST(test_health_json_valid_and_bootloader_slots);
  RUN_TEST(test_health_json_addr_eeprom_bit_surfaces_as_ae);
  RUN_TEST(test_health_json_odometer_emitted_when_valid);
  RUN_TEST(test_odometer_readback_valid_roundtrip);
  RUN_TEST(test_odometer_readback_rejects_old_firmware_garbage);
  RUN_TEST(test_health_json_worst_case_fits_cap_with_reflash_headroom);
  RUN_TEST(test_health_json_combined_splices_fit_cap);
  RUN_TEST(test_health_json_silent_gap_slot);
  RUN_TEST(test_health_json_overflow_reports_full);
  RUN_TEST(test_letter_readback_valid_pair);
  RUN_TEST(test_letter_readback_rejects_bad_complement);
  RUN_TEST(test_letter_readback_rejects_out_of_range);
  RUN_TEST(test_atmega328p_signature);
  RUN_TEST(test_fw_status_matches_bundled_rev);
  RUN_TEST(test_fw_status_outdated_on_mismatch);
  RUN_TEST(test_fw_status_unknown_on_empty_version_or_bundle);
  RUN_TEST(test_fw_status_compares_first_eight_chars_like_v1);
  return UNITY_END();
}
