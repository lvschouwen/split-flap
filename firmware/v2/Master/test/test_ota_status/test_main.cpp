// Host-side unit tests for OtaStatus.h (#190) — the pure decision logic
// behind POST /firmware/master's md5 param and the /settings OTA verdict
// fields synthesized from esp_ota partition state.

#include <ArduinoFake.h>
#include <unity.h>

#include "../../OtaStatus.h"

void setUp() {}
void tearDown() {}

// --- normalizeOtaMd5 ---------------------------------------------------------

static void test_md5_valid_lowercase_passes_unchanged() {
  String md5 = "0123456789abcdef0123456789abcdef";
  TEST_ASSERT_TRUE(normalizeOtaMd5(md5));
  TEST_ASSERT_EQUAL_STRING("0123456789abcdef0123456789abcdef", md5.c_str());
}

static void test_md5_uppercase_is_lowercased() {
  // v1 parity (#144): clients may send uppercase digests; the updater wants
  // lowercase.
  String md5 = "0123456789ABCDEF0123456789ABCDEF";
  TEST_ASSERT_TRUE(normalizeOtaMd5(md5));
  TEST_ASSERT_EQUAL_STRING("0123456789abcdef0123456789abcdef", md5.c_str());
}

static void test_md5_wrong_length_rejected() {
  String s31 = "0123456789abcdef0123456789abcde";
  String s33 = "0123456789abcdef0123456789abcdef0";
  String empty = "";
  TEST_ASSERT_FALSE(normalizeOtaMd5(s31));
  TEST_ASSERT_FALSE(normalizeOtaMd5(s33));
  TEST_ASSERT_FALSE(normalizeOtaMd5(empty));
}

static void test_md5_non_hex_rejected() {
  // Stricter than v1 (which let Update discover the mismatch at end()):
  // a digest that can't be a digest is a client bug — fail fast with 400.
  String md5 = "0123456789abcdef0123456789abcdeg";
  TEST_ASSERT_FALSE(normalizeOtaMd5(md5));
  String spaced = "0123456789abcdef 123456789abcdef";
  TEST_ASSERT_FALSE(normalizeOtaMd5(spaced));
}

// --- synthesizeOtaVerdict ----------------------------------------------------

static void test_plain_boot_yields_empty_verdict() {
  OtaVerdict v = synthesizeOtaVerdict(false, false, false);
  TEST_ASSERT_EQUAL_STRING("", v.lastFlashResult.c_str());
  TEST_ASSERT_FALSE(v.otaReverted);
}

static void test_rollback_yields_reverted() {
  // The bootloader fell back to the other slot: the surviving (old) image
  // reports the verdict ota-master.sh keys on.
  OtaVerdict v = synthesizeOtaVerdict(true, false, false);
  TEST_ASSERT_EQUAL_STRING("reverted", v.lastFlashResult.c_str());
  TEST_ASSERT_TRUE(v.otaReverted);
}

static void test_pending_verify_yields_pending() {
  // Fresh image booted, net not up yet — health confirm hasn't run.
  OtaVerdict v = synthesizeOtaVerdict(false, true, false);
  TEST_ASSERT_EQUAL_STRING("pending", v.lastFlashResult.c_str());
  TEST_ASSERT_FALSE(v.otaReverted);
}

static void test_confirmed_this_boot_yields_ok() {
  OtaVerdict v = synthesizeOtaVerdict(false, true, true);
  TEST_ASSERT_EQUAL_STRING("ok", v.lastFlashResult.c_str());
  TEST_ASSERT_FALSE(v.otaReverted);
}

static void test_fresh_flash_outranks_stale_rollback_history() {
  // Retry flow: attempt 1 reverted, attempt 2 is now running. This boot's
  // own state (pending/confirmed) is the verdict; the lingering invalid
  // mark is history and must not scare the flasher script.
  OtaVerdict pending = synthesizeOtaVerdict(true, true, false);
  TEST_ASSERT_EQUAL_STRING("pending", pending.lastFlashResult.c_str());
  TEST_ASSERT_FALSE(pending.otaReverted);
  OtaVerdict ok = synthesizeOtaVerdict(true, true, true);
  TEST_ASSERT_EQUAL_STRING("ok", ok.lastFlashResult.c_str());
  TEST_ASSERT_FALSE(ok.otaReverted);
}

// ---------------------------------------------------------------------------

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_md5_valid_lowercase_passes_unchanged);
  RUN_TEST(test_md5_uppercase_is_lowercased);
  RUN_TEST(test_md5_wrong_length_rejected);
  RUN_TEST(test_md5_non_hex_rejected);
  RUN_TEST(test_plain_boot_yields_empty_verdict);
  RUN_TEST(test_rollback_yields_reverted);
  RUN_TEST(test_pending_verify_yields_pending);
  RUN_TEST(test_confirmed_this_boot_yields_ok);
  RUN_TEST(test_fresh_flash_outranks_stale_rollback_history);
  return UNITY_END();
}
