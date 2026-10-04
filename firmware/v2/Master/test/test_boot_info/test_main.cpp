// Host-side tests for the read-only unit boot report (BootInfo.h, #499): the
// JSON an operator reads back after a bootloader update, and the seq ordering
// it shares with the other single-slot op results.

#include <cstring>

#include <unity.h>

#include "BootInfo.h"

void setUp() {}
void tearDown() {}

static BootInfoSlot okSlot(uint32_t seq) {
  BootInfoSlot s;
  s.seq = seq;
  s.addr = 3;
  s.done = true;
  s.ok = true;
  s.report.lockByte = 0xFF;
  s.report.fuseLow = 0xFF;
  s.report.fuseHigh = 0xDC;
  s.report.fuseExt = 0xFD;
  s.report.bootCrc32 = 0xE422A668UL;
  s.report.state = BOOT_STATE_NEW;
  s.report.lastResult = BOOT_RESULT_STAGE2_OK;
  return s;
}

static void test_ok_report_carries_every_field() {
  char buf[BOOT_INFO_JSON_CAP];
  size_t n = buildBootInfoJson(buf, sizeof(buf), okSlot(7), 7);
  TEST_ASSERT_EQUAL_STRING(
      "{\"state\":\"ok\",\"addr\":3,\"boot\":\"new\",\"crc32\":\"e422a668\","
      "\"lock\":\"ff\",\"lfuse\":\"ff\",\"hfuse\":\"dc\",\"efuse\":\"fd\","
      "\"last\":\"stage2-ok\"}",
      buf);
  TEST_ASSERT_EQUAL_size_t(strlen(buf), n);
}

static void test_crc_is_zero_padded_hex() {
  BootInfoSlot s = okSlot(1);
  s.report.bootCrc32 = 0x0000ABCDUL;
  s.report.state = BOOT_STATE_OLD;
  s.report.lastResult = BOOT_RESULT_NONE;
  char buf[BOOT_INFO_JSON_CAP];
  buildBootInfoJson(buf, sizeof(buf), s, 1);
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"crc32\":\"0000abcd\""));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"boot\":\"old\""));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"last\":\"none\""));
}

static void test_seq_ordering() {
  char buf[BOOT_INFO_JSON_CAP];
  BootInfoSlot fresh;  // nothing ever read
  buildBootInfoJson(buf, sizeof(buf), fresh, 1);
  TEST_ASSERT_EQUAL_STRING("{\"state\":\"pending\"}", buf);
  // An older result is still in the slot while seq 9 is queued.
  buildBootInfoJson(buf, sizeof(buf), okSlot(8), 9);
  TEST_ASSERT_EQUAL_STRING("{\"state\":\"pending\"}", buf);
  // A newer result replaced the one asked for.
  buildBootInfoJson(buf, sizeof(buf), okSlot(8), 7);
  TEST_ASSERT_EQUAL_STRING("{\"state\":\"expired\"}", buf);
}

static void test_failed_read_never_shows_a_report() {
  // A unit on firmware without the opcode: the read is rejected, and the
  // default-constructed report (state unknown, all-0xFF bytes) must not be
  // served as if the unit had said it.
  BootInfoSlot s;
  s.seq = 4;
  s.addr = 9;
  s.done = true;
  s.ok = false;
  char buf[BOOT_INFO_JSON_CAP];
  buildBootInfoJson(buf, sizeof(buf), s, 4);
  TEST_ASSERT_EQUAL_STRING(
      "{\"state\":\"failed\",\"addr\":9,\"reason\":\"read-fail\"}", buf);
  TEST_ASSERT_NULL(strstr(buf, "crc32"));
}

static void test_every_state_and_result_has_a_distinct_name() {
  // Every state a unit can report, so one added without a name reads
  // "unknown" here instead of on the operator's screen.
  for (int i = 0; i <= BOOT_STATE_LAST; i++) {
    for (int j = i + 1; j <= BOOT_STATE_LAST; j++) {
      TEST_ASSERT_TRUE(strcmp(bootInfoStateName((uint8_t)i),
                              bootInfoStateName((uint8_t)j)) != 0);
    }
    if (i != BOOT_STATE_UNKNOWN) {
      TEST_ASSERT_TRUE(strcmp(bootInfoStateName((uint8_t)i), "unknown") != 0);
    }
  }
  TEST_ASSERT_EQUAL_STRING("prev-new", bootInfoStateName(BOOT_STATE_PREV_NEW));
  for (int r = 0; r < BOOT_RESULT_COUNT; r++) {
    for (int q = r + 1; q < BOOT_RESULT_COUNT; q++) {
      TEST_ASSERT_TRUE(strcmp(bootInfoResultName((uint8_t)r),
                              bootInfoResultName((uint8_t)q)) != 0);
    }
    TEST_ASSERT_TRUE(strcmp(bootInfoResultName((uint8_t)r), "unknown") != 0);
  }
  TEST_ASSERT_EQUAL_STRING("unknown", bootInfoStateName(200));
  TEST_ASSERT_EQUAL_STRING("unknown", bootInfoResultName(200));
}

static void test_longest_reply_fits_the_declared_cap_and_a_small_one_truncates() {
  BootInfoSlot s = okSlot(1);
  s.addr = 126;
  s.report.state = BOOT_STATE_TRAMPOLINE;
  s.report.lastResult = BOOT_RESULT_REFUSED_STATE;
  char buf[BOOT_INFO_JSON_CAP];
  size_t n = buildBootInfoJson(buf, sizeof(buf), s, 1);
  TEST_ASSERT_TRUE(n < BOOT_INFO_JSON_CAP - 1);
  TEST_ASSERT_EQUAL_CHAR('}', buf[n - 1]);
  char tiny[16];
  size_t m = buildBootInfoJson(tiny, sizeof(tiny), s, 1);
  TEST_ASSERT_EQUAL_size_t(sizeof(tiny) - 1, m);
  TEST_ASSERT_EQUAL_size_t(sizeof(tiny) - 1, strlen(tiny));
}

// #518: a unit that cannot read its lock and fuses says so; no bytes are
// printed that a reader could take for values.
static void test_unreadable_lock_and_fuses_are_named_not_printed() {
  BootInfoSlot s = okSlot(2);
  s.report.lockFuseReadable = false;
  s.report.state = BOOT_STATE_OLD;
  s.report.bootCrc32 = 0x18173ADDUL;
  s.report.lastResult = BOOT_RESULT_NONE;
  char buf[BOOT_INFO_JSON_CAP];
  buildBootInfoJson(buf, sizeof(buf), s, 2);
  TEST_ASSERT_EQUAL_STRING(
      "{\"state\":\"ok\",\"addr\":3,\"boot\":\"old\",\"crc32\":\"18173add\","
      "\"lockfuse\":\"unreadable\",\"last\":\"none\"}",
      buf);
  TEST_ASSERT_NULL(strstr(buf, "\"lock\":"));
  TEST_ASSERT_NULL(strstr(buf, "fuse\":\"f"));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_ok_report_carries_every_field);
  RUN_TEST(test_crc_is_zero_padded_hex);
  RUN_TEST(test_seq_ordering);
  RUN_TEST(test_failed_read_never_shows_a_report);
  RUN_TEST(test_every_state_and_result_has_a_distinct_name);
  RUN_TEST(test_longest_reply_fits_the_declared_cap_and_a_small_one_truncates);
  RUN_TEST(test_unreadable_lock_and_fuses_are_named_not_printed);
  return UNITY_END();
}
