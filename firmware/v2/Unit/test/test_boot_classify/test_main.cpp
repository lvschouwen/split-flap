// Host-side tests for the pure boot-section classifier (BootSectionClassify.h,
// #499): from a few cheaply-computed facts about the unit's own boot section,
// decide which twiboot-update state it is in. The SPM glue that reads those
// facts and acts on the verdict is bench/sim tier.

#include <unity.h>
#include <stdint.h>
#include "BootSectionClassify.h"

void setUp() {}
void tearDown() {}

// Stand-in constants from the generated new-image header. The classifier takes
// them as parameters so it never embeds build-specific values; the real caller
// passes NEW_TWIBOOT_CRC32 / NEW_TWIBOOT_PAGE7_INSTALLED_CRC32.
static const uint32_t NEW_CRC = 0x333aaf99UL;
static const uint32_t P7_CRC = 0x12345678UL;  // fielded 0-6 + new page 7
// A pages-0-6 CRC that is NOT the fielded core, so the retriable fallback does
// not fire unless a test opts in with BOOT_FIELDED_PAGES_0_6_CRC32.
static const uint32_t NOT_FIELDED_P06 = 0xA5A5A5A5UL;

static BootSectionFacts facts(uint32_t crc, uint16_t w0, uint16_t w1) {
  // Default pages-0-6 CRC to a non-fielded value; the dirty-page-7 test sets it.
  BootSectionFacts f;
  f.fullCrc32 = crc;
  f.pages0_6Crc32 = NOT_FIELDED_P06;
  f.page0Word0 = w0;
  f.page0Word1 = w1;
  return f;
}

// --- the three clean, CRC-identified states --------------------------------

static void test_old_is_fielded_crc() {
  // Fielded image: real twiboot first instruction at page 0, not a trampoline.
  TEST_ASSERT_EQUAL(BOOT_STATE_OLD,
                    classifyBootSection(facts(BOOT_FIELDED_CRC32, 0x24c0, 0),
                                        NEW_CRC, P7_CRC));
}

static void test_new_is_new_crc() {
  TEST_ASSERT_EQUAL(BOOT_STATE_NEW,
                    classifyBootSection(facts(NEW_CRC, 0x24c0, 0), NEW_CRC,
                                        P7_CRC));
}

static void test_page7_installed_is_its_crc() {
  TEST_ASSERT_EQUAL(BOOT_STATE_PAGE7_INSTALLED,
                    classifyBootSection(facts(P7_CRC, 0x24c0, 0), NEW_CRC,
                                        P7_CRC));
}

// --- trampoline: page 0 is a jmp-to-app, CRC matches no clean image ---------

static void test_trampoline_detected_by_page0_jmp() {
  TEST_ASSERT_EQUAL(
      BOOT_STATE_TRAMPOLINE,
      classifyBootSection(facts(0xdeadbeefUL, BOOT_TRAMPOLINE_WORD0,
                                BOOT_TRAMPOLINE_WORD1),
                          NEW_CRC, P7_CRC));
}

static void test_trampoline_needs_both_words() {
  // jmp opcode but wrong target word is not a recognized trampoline.
  TEST_ASSERT_EQUAL(
      BOOT_STATE_UNKNOWN,
      classifyBootSection(facts(0xdeadbeefUL, BOOT_TRAMPOLINE_WORD0, 0x0001),
                          NEW_CRC, P7_CRC));
}

// --- precedence: a clean CRC wins over a stray trampoline-looking page 0 ----

static void test_clean_crc_beats_trampoline_marker() {
  // Should never happen on real silicon, but if a clean image somehow also had
  // a jmp-0 at page 0, the CRC identity must win (New/Old/Page7 are terminal).
  TEST_ASSERT_EQUAL(
      BOOT_STATE_NEW,
      classifyBootSection(facts(NEW_CRC, BOOT_TRAMPOLINE_WORD0,
                                BOOT_TRAMPOLINE_WORD1),
                          NEW_CRC, P7_CRC));
}

// --- fielded core + dirty page 7 is retriable (reports Old) ------------------

static void test_dirty_page7_with_fielded_core_is_old() {
  // A half-done/interrupted stage 1: pages 0-6 are still the fielded twiboot but
  // the whole-section CRC matches nothing (page 7 is partially written). Must
  // classify Old so stage 1 can be retried rather than bricking to ICSP.
  BootSectionFacts f = facts(0xdeadbeefUL, 0x24c0, 0);
  f.pages0_6Crc32 = BOOT_FIELDED_PAGES_0_6_CRC32;
  TEST_ASSERT_EQUAL(BOOT_STATE_OLD, classifyBootSection(f, NEW_CRC, P7_CRC));
}

static void test_dirty_page7_without_fielded_core_is_unknown() {
  // If pages 0-6 are NOT the fielded core, it is not a safe stage-1 retry.
  BootSectionFacts f = facts(0xdeadbeefUL, 0x24c0, 0);  // default non-fielded p0-6
  TEST_ASSERT_EQUAL(BOOT_STATE_UNKNOWN, classifyBootSection(f, NEW_CRC, P7_CRC));
}

// --- previous new: a known prior image, stage-2-only path -------------------

static void test_prev_new_is_prev_new_crc() {
  TEST_ASSERT_EQUAL(BOOT_STATE_PREV_NEW,
                    classifyBootSection(facts(BOOT_PREV_NEW_CRC32, 0x24c0, 0),
                                        NEW_CRC, P7_CRC));
}

static void test_prev_new2_is_prev_new() {
  TEST_ASSERT_EQUAL(BOOT_STATE_PREV_NEW,
                    classifyBootSection(facts(BOOT_PREV_NEW2_CRC32, 0x24c0, 0),
                                        NEW_CRC, P7_CRC));
}

static void test_prev_new_does_not_shadow_current_new() {
  // If the current image CRC happens to equal BOOT_PREV_NEW_CRC32 (only when
  // test stand-ins match), NEW wins because it is checked first.
  TEST_ASSERT_EQUAL(BOOT_STATE_NEW,
                    classifyBootSection(facts(BOOT_PREV_NEW_CRC32, 0x24c0, 0),
                                        BOOT_PREV_NEW_CRC32, P7_CRC));
}

// --- anything else is Unknown and refuses every update ----------------------

static void test_garbage_is_unknown() {
  TEST_ASSERT_EQUAL(BOOT_STATE_UNKNOWN,
                    classifyBootSection(facts(0x00000000UL, 0xffff, 0xffff),
                                        NEW_CRC, P7_CRC));
  TEST_ASSERT_EQUAL(BOOT_STATE_UNKNOWN,
                    classifyBootSection(facts(0xffffffffUL, 0x24c0, 0x0010),
                                        NEW_CRC, P7_CRC));
}

// --- lock-bit gate for boot-section SPM -------------------------------------

static void test_lock_permits_when_blb11_set() {
  TEST_ASSERT_TRUE(bootLockPermitsBootWrite(0xFF));   // chip-erase default
  TEST_ASSERT_TRUE(bootLockPermitsBootWrite(0x10));   // only BLB11 set
}

static void test_lock_refuses_when_blb11_clear() {
  TEST_ASSERT_FALSE(bootLockPermitsBootWrite(0x00));
  TEST_ASSERT_FALSE(bootLockPermitsBootWrite(0xEF));  // everything but BLB11
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_old_is_fielded_crc);
  RUN_TEST(test_lock_permits_when_blb11_set);
  RUN_TEST(test_lock_refuses_when_blb11_clear);
  RUN_TEST(test_new_is_new_crc);
  RUN_TEST(test_page7_installed_is_its_crc);
  RUN_TEST(test_trampoline_detected_by_page0_jmp);
  RUN_TEST(test_trampoline_needs_both_words);
  RUN_TEST(test_clean_crc_beats_trampoline_marker);
  RUN_TEST(test_prev_new_is_prev_new_crc);
  RUN_TEST(test_prev_new2_is_prev_new);
  RUN_TEST(test_prev_new_does_not_shadow_current_new);
  RUN_TEST(test_dirty_page7_with_fielded_core_is_old);
  RUN_TEST(test_dirty_page7_without_fielded_core_is_unknown);
  RUN_TEST(test_garbage_is_unknown);
  return UNITY_END();
}
