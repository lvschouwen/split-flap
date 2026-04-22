// Host-side tests for RtcBootState.h.
//
// Pure data-layout + cookie-handling logic from the master's RTC boot
// state. The hardware-only rtcUserMemoryRead/Write wrappers live in
// ESPMaster.ino and are out of scope here — this file only exercises
// the pieces a host compiler can run.
//
// Motivation (theory 2 in the #53 crash hunt): the original cookie
// resolution did a raw `String == char*` compare on the 36-byte slot,
// which walks until the first NUL. If RTC was ever written by a foreign
// firmware that happened to match RTC_BOOT_MAGIC, or the magic check
// hadn't been run before dereference, the compare could read past the
// struct. These tests lock in the bounded helpers that close that gap.

#include <ArduinoFake.h>
#include <unity.h>

#include <cstring>

#include "../../RtcBootState.h"

using namespace fakeit;

void setUp() {
  ArduinoFakeReset();
}
void tearDown() {}

// --- layout invariants --------------------------------------------------

static void test_struct_size_is_multiple_of_4() {
  // rtcUserMemoryRead/Write require block-aligned sizes (4-byte blocks).
  TEST_ASSERT_EQUAL_INT(0, (int)(sizeof(RtcBootState) % 4));
}

static void test_struct_size_matches_expected_layout() {
  // 4 (magic) + 4 (bootCounter) + 36 (preFlashSketchMd5) = 44. Locking
  // this in prevents silent padding changes from an unrelated struct
  // edit.
  TEST_ASSERT_EQUAL_INT(44, (int)sizeof(RtcBootState));
}

static void test_pre_flash_md5_len_fits_hex_plus_nul() {
  // 32-char MD5 hex + NUL = 33. Slot must have room for that plus
  // alignment padding.
  TEST_ASSERT_GREATER_OR_EQUAL_INT(33, (int)PRE_FLASH_MD5_LEN);
}

static void test_magic_matches_v2_value() {
  // Lock in the RTC magic — bumping it silently would break boot-state
  // compatibility with any device already running the current firmware.
  TEST_ASSERT_EQUAL_UINT32(0xC0FFEE43UL, RTC_BOOT_MAGIC);
}

// --- normalizeBootState -------------------------------------------------

static void test_normalize_zeros_state_when_magic_mismatch() {
  RtcBootState state;
  state.magic = 0xDEADBEEFUL;
  state.bootCounter = 42;
  memset(state.preFlashSketchMd5, 0xAA, PRE_FLASH_MD5_LEN);

  normalizeBootState(state);

  TEST_ASSERT_EQUAL_UINT32(RTC_BOOT_MAGIC, state.magic);
  TEST_ASSERT_EQUAL_UINT32(0, state.bootCounter);
  for (size_t i = 0; i < PRE_FLASH_MD5_LEN; i++) {
    TEST_ASSERT_EQUAL_UINT8(0, state.preFlashSketchMd5[i]);
  }
}

static void test_normalize_preserves_state_when_magic_matches() {
  RtcBootState state;
  state.magic = RTC_BOOT_MAGIC;
  state.bootCounter = 7;
  memset(state.preFlashSketchMd5, 0, PRE_FLASH_MD5_LEN);
  const char* md5 = "0123456789abcdef0123456789abcdef";
  memcpy(state.preFlashSketchMd5, md5, 32);

  normalizeBootState(state);

  TEST_ASSERT_EQUAL_UINT32(RTC_BOOT_MAGIC, state.magic);
  TEST_ASSERT_EQUAL_UINT32(7, state.bootCounter);
  TEST_ASSERT_EQUAL_INT(0, memcmp(state.preFlashSketchMd5, md5, 32));
}

// --- setPreFlashMd5 -----------------------------------------------------

static void test_setPreFlashMd5_typical_32_char_md5() {
  RtcBootState state;
  memset(&state, 0xAA, sizeof(state));  // arbitrary non-zero background
  const char* md5 = "0123456789abcdef0123456789abcdef";

  setPreFlashMd5(state, md5);

  TEST_ASSERT_EQUAL_INT(0, memcmp(state.preFlashSketchMd5, md5, 32));
  TEST_ASSERT_EQUAL_UINT8(0, state.preFlashSketchMd5[32]);
  // Tail bytes must be zeroed so a SerialPrint that walks past the NUL
  // doesn't surface leftover 0xAA fill from before the call.
  for (size_t i = 33; i < PRE_FLASH_MD5_LEN; i++) {
    TEST_ASSERT_EQUAL_UINT8(0, state.preFlashSketchMd5[i]);
  }
}

static void test_setPreFlashMd5_empty_string() {
  RtcBootState state;
  memset(&state, 0xAA, sizeof(state));

  setPreFlashMd5(state, "");

  TEST_ASSERT_EQUAL_UINT8(0, state.preFlashSketchMd5[0]);
  for (size_t i = 1; i < PRE_FLASH_MD5_LEN; i++) {
    TEST_ASSERT_EQUAL_UINT8(0, state.preFlashSketchMd5[i]);
  }
}

static void test_setPreFlashMd5_null_pointer() {
  RtcBootState state;
  memset(&state, 0xAA, sizeof(state));

  setPreFlashMd5(state, nullptr);

  for (size_t i = 0; i < PRE_FLASH_MD5_LEN; i++) {
    TEST_ASSERT_EQUAL_UINT8(0, state.preFlashSketchMd5[i]);
  }
}

static void test_setPreFlashMd5_overlong_input_truncates_and_terminates() {
  RtcBootState state;
  memset(&state, 0xAA, sizeof(state));
  // 50 chars, longer than PRE_FLASH_MD5_LEN (36).
  const char* tooLong = "aaaaaaaaaabbbbbbbbbbccccccccccddddddddddeeeeeeeeee";

  setPreFlashMd5(state, tooLong);

  // First 35 bytes match the source, byte 35 is the NUL terminator.
  TEST_ASSERT_EQUAL_INT(0, memcmp(state.preFlashSketchMd5, tooLong, PRE_FLASH_MD5_LEN - 1));
  TEST_ASSERT_EQUAL_UINT8(0, state.preFlashSketchMd5[PRE_FLASH_MD5_LEN - 1]);
}

static void test_setPreFlashMd5_overwrites_longer_prior_content() {
  // Regression for the leak-past-terminator scenario: writing "abc" after
  // "defghijklmnop..." must zero the tail so a later SerialPrint walking
  // until NUL doesn't see "abc\0ghijklmnop...".
  RtcBootState state;
  memset(&state, 0, sizeof(state));
  const char* longFirst = "defghijklmnopqrstuvwxyz0123456789";
  setPreFlashMd5(state, longFirst);

  setPreFlashMd5(state, "abc");

  TEST_ASSERT_EQUAL_INT(0, memcmp(state.preFlashSketchMd5, "abc", 3));
  // Everything from index 3 onward must be 0, including the bytes that
  // used to hold longFirst's tail.
  for (size_t i = 3; i < PRE_FLASH_MD5_LEN; i++) {
    TEST_ASSERT_EQUAL_UINT8(0, state.preFlashSketchMd5[i]);
  }
}

// --- cookieIsPresent / cookieLength / cookieIsMalformed ----------------

static void test_cookieIsPresent_false_on_zero_state() {
  RtcBootState state;
  memset(&state, 0, sizeof(state));
  TEST_ASSERT_FALSE(cookieIsPresent(state));
}

static void test_cookieIsPresent_true_when_first_byte_nonzero() {
  RtcBootState state;
  memset(&state, 0, sizeof(state));
  state.preFlashSketchMd5[0] = 'a';
  TEST_ASSERT_TRUE(cookieIsPresent(state));
}

static void test_cookieLength_empty_is_zero() {
  RtcBootState state;
  memset(&state, 0, sizeof(state));
  TEST_ASSERT_EQUAL_INT(0, (int)cookieLength(state));
}

static void test_cookieLength_well_formed_md5_is_32() {
  RtcBootState state;
  memset(&state, 0, sizeof(state));
  setPreFlashMd5(state, "0123456789abcdef0123456789abcdef");
  TEST_ASSERT_EQUAL_INT(32, (int)cookieLength(state));
}

static void test_cookieLength_unterminated_returns_slot_size() {
  // Every byte non-zero, no NUL anywhere. cookieLength must NOT walk
  // past the slot — it returns PRE_FLASH_MD5_LEN as the "malformed"
  // sentinel.
  RtcBootState state;
  memset(&state, 0, sizeof(state));
  memset(state.preFlashSketchMd5, 'Z', PRE_FLASH_MD5_LEN);
  TEST_ASSERT_EQUAL_INT(PRE_FLASH_MD5_LEN, (int)cookieLength(state));
  TEST_ASSERT_TRUE(cookieIsMalformed(state));
}

// --- cookieMatchesRunning -----------------------------------------------

static void test_cookieMatchesRunning_exact_match_is_true() {
  RtcBootState state;
  memset(&state, 0, sizeof(state));
  const char* md5 = "0123456789abcdef0123456789abcdef";
  setPreFlashMd5(state, md5);

  TEST_ASSERT_TRUE(cookieMatchesRunning(state, md5));
}

static void test_cookieMatchesRunning_different_content_is_false() {
  RtcBootState state;
  memset(&state, 0, sizeof(state));
  setPreFlashMd5(state, "0123456789abcdef0123456789abcdef");
  const char* different = "ffffffffffffffffffffffffffffffff";

  TEST_ASSERT_FALSE(cookieMatchesRunning(state, different));
}

static void test_cookieMatchesRunning_empty_cookie_is_false() {
  RtcBootState state;
  memset(&state, 0, sizeof(state));
  TEST_ASSERT_FALSE(cookieMatchesRunning(state, "0123456789abcdef0123456789abcdef"));
}

static void test_cookieMatchesRunning_null_running_is_false() {
  RtcBootState state;
  memset(&state, 0, sizeof(state));
  setPreFlashMd5(state, "0123456789abcdef0123456789abcdef");
  TEST_ASSERT_FALSE(cookieMatchesRunning(state, nullptr));
}

static void test_cookieMatchesRunning_length_mismatch_is_false() {
  // Running MD5 is a longer string with the cookie as a prefix — a naive
  // strncmp would return true; bounded compare must check full length.
  RtcBootState state;
  memset(&state, 0, sizeof(state));
  setPreFlashMd5(state, "abcdef");
  TEST_ASSERT_FALSE(cookieMatchesRunning(state, "abcdefghij"));
}

static void test_cookieMatchesRunning_unterminated_cookie_is_false() {
  // Theory 2 core assertion: a 36-byte cookie with no NUL must be treated
  // as no-match, NOT a read-past-end UB. This is the guarantee the original
  // `String == char*` compare lacked.
  RtcBootState state;
  memset(&state, 0, sizeof(state));
  memset(state.preFlashSketchMd5, 'Z', PRE_FLASH_MD5_LEN);
  TEST_ASSERT_FALSE(cookieMatchesRunning(state, "ZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZ"));
  TEST_ASSERT_FALSE(cookieMatchesRunning(state, "running-md5"));
}

// --- main ---------------------------------------------------------------

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_struct_size_is_multiple_of_4);
  RUN_TEST(test_struct_size_matches_expected_layout);
  RUN_TEST(test_pre_flash_md5_len_fits_hex_plus_nul);
  RUN_TEST(test_magic_matches_v2_value);
  RUN_TEST(test_normalize_zeros_state_when_magic_mismatch);
  RUN_TEST(test_normalize_preserves_state_when_magic_matches);
  RUN_TEST(test_setPreFlashMd5_typical_32_char_md5);
  RUN_TEST(test_setPreFlashMd5_empty_string);
  RUN_TEST(test_setPreFlashMd5_null_pointer);
  RUN_TEST(test_setPreFlashMd5_overlong_input_truncates_and_terminates);
  RUN_TEST(test_setPreFlashMd5_overwrites_longer_prior_content);
  RUN_TEST(test_cookieIsPresent_false_on_zero_state);
  RUN_TEST(test_cookieIsPresent_true_when_first_byte_nonzero);
  RUN_TEST(test_cookieLength_empty_is_zero);
  RUN_TEST(test_cookieLength_well_formed_md5_is_32);
  RUN_TEST(test_cookieLength_unterminated_returns_slot_size);
  RUN_TEST(test_cookieMatchesRunning_exact_match_is_true);
  RUN_TEST(test_cookieMatchesRunning_different_content_is_false);
  RUN_TEST(test_cookieMatchesRunning_empty_cookie_is_false);
  RUN_TEST(test_cookieMatchesRunning_null_running_is_false);
  RUN_TEST(test_cookieMatchesRunning_length_mismatch_is_false);
  RUN_TEST(test_cookieMatchesRunning_unterminated_cookie_is_false);
  return UNITY_END();
}
