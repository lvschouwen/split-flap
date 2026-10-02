// Host-side unit tests for BootDump.h (#511) — the pure half of the unit
// boot-section dump: result slot, CRC and the /unit/boot-dump-result JSON.

#include <ArduinoFake.h>
#include <string.h>
#include <unity.h>

#include "../../BootDump.h"

void setUp() {}
void tearDown() {}

static BootDumpSlot slot(uint32_t seq, BootDumpOutcome o) {
  BootDumpSlot s;
  s.seq = seq;
  s.addr = 5;
  s.outcome = o;
  s.crc32 = 0xDEADBEEFUL;
  return s;
}

static void test_boot_section_is_the_328p_1k_section() {
  TEST_ASSERT_EQUAL_HEX16(0x7C00, BOOT_SECTION_START);
  TEST_ASSERT_EQUAL(1024, BOOT_SECTION_LEN);
}

static void test_crc32_matches_the_standard_check_value() {
  // The zlib/PNG CRC-32, so a host can compare with python's zlib.crc32.
  TEST_ASSERT_EQUAL_HEX32(0xCBF43926UL,
                          bootDumpCrc32((const uint8_t*)"123456789", 9));
}

static void test_crc32_of_nothing_is_zero() {
  TEST_ASSERT_EQUAL_HEX32(0, bootDumpCrc32(nullptr, 0));
}

static void test_every_outcome_has_a_distinct_name() {
  const BootDumpOutcome all[] = {
      BootDumpOutcome::Ok,          BootDumpOutcome::EnterFail,
      BootDumpOutcome::BootloaderSilent, BootDumpOutcome::ChipMismatch,
      BootDumpOutcome::ReadFail};
  for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
    TEST_ASSERT_TRUE(strcmp(bootDumpOutcomeName(all[i]), "pending") != 0);
    for (size_t j = i + 1; j < sizeof(all) / sizeof(all[0]); j++) {
      TEST_ASSERT_TRUE(strcmp(bootDumpOutcomeName(all[i]),
                              bootDumpOutcomeName(all[j])) != 0);
    }
  }
}

static void test_older_slot_reads_pending() {
  char buf[BOOT_DUMP_JSON_CAP];
  buildBootDumpJson(buf, sizeof(buf), slot(3, BootDumpOutcome::Ok), 4, nullptr);
  TEST_ASSERT_EQUAL_STRING("{\"state\":\"pending\"}", buf);
}

static void test_same_seq_still_pending_reads_pending() {
  char buf[BOOT_DUMP_JSON_CAP];
  buildBootDumpJson(buf, sizeof(buf), slot(4, BootDumpOutcome::Pending), 4,
                    nullptr);
  TEST_ASSERT_EQUAL_STRING("{\"state\":\"pending\"}", buf);
}

static void test_newer_slot_reads_expired() {
  char buf[BOOT_DUMP_JSON_CAP];
  buildBootDumpJson(buf, sizeof(buf), slot(9, BootDumpOutcome::Ok), 4, nullptr);
  TEST_ASSERT_EQUAL_STRING("{\"state\":\"expired\"}", buf);
}

static void test_failure_names_its_reason() {
  char buf[BOOT_DUMP_JSON_CAP];
  buildBootDumpJson(buf, sizeof(buf), slot(4, BootDumpOutcome::ChipMismatch),
                    4, nullptr);
  TEST_ASSERT_EQUAL_STRING(
      "{\"state\":\"failed\",\"addr\":5,\"reason\":\"chip-mismatch\"}", buf);
}

static void test_ok_without_the_bytes_is_expired() {
  // The byte store was overwritten by a later dump between the snapshot and
  // the copy: the slot's bytes are gone, so it must not claim them.
  char buf[BOOT_DUMP_JSON_CAP];
  buildBootDumpJson(buf, sizeof(buf), slot(4, BootDumpOutcome::Ok), 4, nullptr);
  TEST_ASSERT_EQUAL_STRING("{\"state\":\"expired\"}", buf);
}

static void test_ok_carries_crc_and_all_bytes_as_hex() {
  static uint8_t bytes[BOOT_SECTION_LEN];
  for (int i = 0; i < BOOT_SECTION_LEN; i++) bytes[i] = (uint8_t)(i * 7 + 1);
  static char buf[BOOT_DUMP_JSON_CAP];
  size_t n = buildBootDumpJson(buf, sizeof(buf), slot(4, BootDumpOutcome::Ok),
                               4, bytes);
  TEST_ASSERT_EQUAL(strlen(buf), n);
  TEST_ASSERT_NOT_NULL(strstr(
      buf, "{\"state\":\"ok\",\"addr\":5,\"start\":31744,\"len\":1024,"
           "\"crc32\":\"deadbeef\",\"hex\":\""));
  const char* hex = strstr(buf, "\"hex\":\"") + 7;
  TEST_ASSERT_EQUAL(2 * BOOT_SECTION_LEN, strcspn(hex, "\""));
  TEST_ASSERT_EQUAL_STRING_LEN("01080f16", hex, 8);        // first bytes
  TEST_ASSERT_EQUAL_STRING_LEN("fa", hex + 2 * 1023, 2);   // last byte
  TEST_ASSERT_EQUAL_STRING("\"}", hex + 2 * BOOT_SECTION_LEN);
}

static void test_ok_never_overruns_a_short_buffer() {
  static uint8_t bytes[BOOT_SECTION_LEN];
  char buf[64];
  memset(buf, 0x55, sizeof(buf));
  size_t n = buildBootDumpJson(buf, 32, slot(4, BootDumpOutcome::Ok), 4, bytes);
  TEST_ASSERT_TRUE(n < 32);
  TEST_ASSERT_EQUAL_UINT8(0x55, (uint8_t)buf[32]);
  TEST_ASSERT_EQUAL(strlen(buf), n);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_boot_section_is_the_328p_1k_section);
  RUN_TEST(test_crc32_matches_the_standard_check_value);
  RUN_TEST(test_crc32_of_nothing_is_zero);
  RUN_TEST(test_every_outcome_has_a_distinct_name);
  RUN_TEST(test_older_slot_reads_pending);
  RUN_TEST(test_same_seq_still_pending_reads_pending);
  RUN_TEST(test_newer_slot_reads_expired);
  RUN_TEST(test_failure_names_its_reason);
  RUN_TEST(test_ok_without_the_bytes_is_expired);
  RUN_TEST(test_ok_carries_crc_and_all_bytes_as_hex);
  RUN_TEST(test_ok_never_overruns_a_short_buffer);
  return UNITY_END();
}
