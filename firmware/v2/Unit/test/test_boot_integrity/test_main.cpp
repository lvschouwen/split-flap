// Host-side tests for the boot-section integrity watch (BootIntegrity.h, #520):
// when an idle unit recomputes its boot report, how a row master judges the
// report, and when that verdict is worth a log line.

#include <unity.h>
#include <stdint.h>
#include "BootIntegrity.h"

void setUp() {}
void tearDown() {}

static BootUpdateReport report(uint32_t crc, uint8_t state) {
  BootUpdateReport r;
  r.bootCrc32 = crc;
  r.state = state;
  return r;
}

// --- unit: refresh schedule ---------------------------------------------------

static void test_refresh_waits_out_the_interval() {
  TEST_ASSERT_FALSE(bootInfoRefreshDue(0, 0, true));
  TEST_ASSERT_FALSE(
      bootInfoRefreshDue(BOOT_INFO_REFRESH_INTERVAL_MS - 1, 0, true));
  TEST_ASSERT_TRUE(bootInfoRefreshDue(BOOT_INFO_REFRESH_INTERVAL_MS, 0, true));
}

static void test_refresh_never_runs_while_the_drum_is_busy() {
  TEST_ASSERT_FALSE(
      bootInfoRefreshDue(10 * BOOT_INFO_REFRESH_INTERVAL_MS, 0, false));
}

static void test_refresh_survives_the_millis_wrap() {
  uint32_t last = 0xFFFFFFFFUL - 1000UL;
  TEST_ASSERT_FALSE(bootInfoRefreshDue(last + 2000UL, last, true));  // wrapped
  TEST_ASSERT_TRUE(
      bootInfoRefreshDue(last + BOOT_INFO_REFRESH_INTERVAL_MS, last, true));
}

// --- master: verdict ----------------------------------------------------------

static void test_the_expected_image_is_ok() {
  TEST_ASSERT_EQUAL(BOOT_INTEGRITY_OK,
                    bootIntegrityJudge(report(BOOT_CURRENT_CRC32, BOOT_STATE_NEW),
                                       BOOT_CURRENT_CRC32));
}

static void test_one_flipped_bit_is_corrupt() {
  // What rot looks like: the unit matches nothing it knows.
  TEST_ASSERT_EQUAL(
      BOOT_INTEGRITY_CORRUPT,
      bootIntegrityJudge(report(BOOT_CURRENT_CRC32 ^ 1UL, BOOT_STATE_UNKNOWN),
                         BOOT_CURRENT_CRC32));
}

static void test_known_other_images_are_outdated_not_corrupt() {
  TEST_ASSERT_EQUAL(BOOT_INTEGRITY_OUTDATED,
                    bootIntegrityJudge(report(BOOT_FIELDED_CRC32, BOOT_STATE_OLD),
                                       BOOT_CURRENT_CRC32));
  // Mid-update steps have CRCs of their own.
  TEST_ASSERT_EQUAL(
      BOOT_INTEGRITY_OUTDATED,
      bootIntegrityJudge(report(0x12345678UL, BOOT_STATE_PAGE7_INSTALLED),
                         BOOT_CURRENT_CRC32));
  TEST_ASSERT_EQUAL(
      BOOT_INTEGRITY_OUTDATED,
      bootIntegrityJudge(report(0x12345678UL, BOOT_STATE_TRAMPOLINE),
                         BOOT_CURRENT_CRC32));
  // A unit whose firmware installs a newer image than this master knows.
  TEST_ASSERT_EQUAL(BOOT_INTEGRITY_OUTDATED,
                    bootIntegrityJudge(report(0x12345678UL, BOOT_STATE_NEW),
                                       BOOT_CURRENT_CRC32));
}

static void test_the_crc_outranks_the_units_own_label() {
  TEST_ASSERT_EQUAL(
      BOOT_INTEGRITY_OK,
      bootIntegrityJudge(report(BOOT_CURRENT_CRC32, BOOT_STATE_UNKNOWN),
                         BOOT_CURRENT_CRC32));
}

// --- master: log edge ---------------------------------------------------------

static void test_a_first_ok_reading_is_silent() {
  BootIntegrityEdge e = bootIntegrityEdge(BOOT_INTEGRITY_UNREAD, BOOT_INTEGRITY_OK);
  TEST_ASSERT_FALSE(e.log);
  TEST_ASSERT_EQUAL(BOOT_INTEGRITY_OK, e.logged);
}

static void test_a_first_bad_reading_is_logged_once() {
  BootIntegrityEdge e =
      bootIntegrityEdge(BOOT_INTEGRITY_UNREAD, BOOT_INTEGRITY_CORRUPT);
  TEST_ASSERT_TRUE(e.log);
  e = bootIntegrityEdge(e.logged, BOOT_INTEGRITY_CORRUPT);
  TEST_ASSERT_FALSE(e.log);
  TEST_ASSERT_EQUAL(BOOT_INTEGRITY_CORRUPT, e.logged);
}

static void test_going_bad_and_coming_back_each_log() {
  BootIntegrityEdge e = bootIntegrityEdge(BOOT_INTEGRITY_OK, BOOT_INTEGRITY_CORRUPT);
  TEST_ASSERT_TRUE(e.log);
  e = bootIntegrityEdge(e.logged, BOOT_INTEGRITY_OK);
  TEST_ASSERT_TRUE(e.log);
  TEST_ASSERT_EQUAL(BOOT_INTEGRITY_OK, e.logged);
}

static void test_a_missed_read_neither_clears_nor_repeats() {
  BootIntegrityEdge e =
      bootIntegrityEdge(BOOT_INTEGRITY_CORRUPT, BOOT_INTEGRITY_UNREAD);
  TEST_ASSERT_FALSE(e.log);
  TEST_ASSERT_EQUAL(BOOT_INTEGRITY_CORRUPT, e.logged);
  e = bootIntegrityEdge(e.logged, BOOT_INTEGRITY_CORRUPT);
  TEST_ASSERT_FALSE(e.log);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_refresh_waits_out_the_interval);
  RUN_TEST(test_refresh_never_runs_while_the_drum_is_busy);
  RUN_TEST(test_refresh_survives_the_millis_wrap);
  RUN_TEST(test_the_expected_image_is_ok);
  RUN_TEST(test_one_flipped_bit_is_corrupt);
  RUN_TEST(test_known_other_images_are_outdated_not_corrupt);
  RUN_TEST(test_the_crc_outranks_the_units_own_label);
  RUN_TEST(test_a_first_ok_reading_is_silent);
  RUN_TEST(test_a_first_bad_reading_is_logged_once);
  RUN_TEST(test_going_bad_and_coming_back_each_log);
  RUN_TEST(test_a_missed_read_neither_clears_nor_repeats);
  return UNITY_END();
}
