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

// --- otaShouldBlankIntendedVersion (#390) ----------------------------------

static void test_intended_matching_running_rev_is_kept() {
  // The normal case after every successful OTA: ?v= names the image that
  // is now running. Nothing to blank — and no NVS write every boot.
  OtaVerdict clean = synthesizeOtaVerdict(false, false, false);
  TEST_ASSERT_FALSE(otaShouldBlankIntendedVersion("fa7067a", "fa7067a", clean));
  TEST_ASSERT_FALSE(otaShouldBlankIntendedVersion("", "fa7067a", clean));
}

static void test_mismatch_on_clean_boot_blanks() {
  // The #389 bench observation: rescue recovery put fa7067a on app0 while
  // intendedVersion still said f14a7b6 from the last normal OTA. Clean
  // verdict + mismatch = out-of-band recovery — blank the stale intent.
  OtaVerdict clean = synthesizeOtaVerdict(false, false, false);
  TEST_ASSERT_TRUE(otaShouldBlankIntendedVersion("f14a7b6", "fa7067a", clean));
  OtaVerdict confirmed = synthesizeOtaVerdict(false, true, true);
  TEST_ASSERT_TRUE(
      otaShouldBlankIntendedVersion("f14a7b6", "fa7067a", confirmed));
}

static void test_mismatch_on_reverted_boot_is_kept() {
  // A silent revert is EXACTLY version != intendedVersion on an
  // otaReverted verdict — that mismatch is the field's whole purpose and
  // must survive. Gate on the synthesized verdict, not the raw rolledBack
  // flag: a corpse-mark boot (rolledBack but pending/confirmed) is not a
  // revert and does blank (covered above via synthesizeOtaVerdict).
  OtaVerdict reverted = synthesizeOtaVerdict(true, false, false);
  TEST_ASSERT_TRUE(reverted.otaReverted);
  TEST_ASSERT_FALSE(
      otaShouldBlankIntendedVersion("f14a7b6", "fa7067a", reverted));
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
  RUN_TEST(test_intended_matching_running_rev_is_kept);
  RUN_TEST(test_mismatch_on_clean_boot_blanks);
  RUN_TEST(test_mismatch_on_reverted_boot_is_kept);
  return UNITY_END();
}
