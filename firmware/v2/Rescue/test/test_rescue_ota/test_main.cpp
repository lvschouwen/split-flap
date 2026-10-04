// Host-side unit tests for the shared OtaUploadGate.h as the rescue app's POST
// /firmware/master uses it (#195): the md5 contract, the pre-flash gate, the
// completion verdict and the stall rule.

#include <ArduinoFake.h>
#include <unity.h>

#include "OtaUploadGate.h"

void setUp() {}
void tearDown() {}

static void test_md5_lowercase_hex_accepted() {
  String md5 = "0123456789abcdef0123456789abcdef";
  TEST_ASSERT_TRUE(normalizeOtaMd5(md5));
  TEST_ASSERT_EQUAL_STRING("0123456789abcdef0123456789abcdef", md5.c_str());
}

static void test_md5_uppercase_normalized_in_place() {
  String md5 = "0123456789ABCDEF0123456789ABCDEF";
  TEST_ASSERT_TRUE(normalizeOtaMd5(md5));
  TEST_ASSERT_EQUAL_STRING("0123456789abcdef0123456789abcdef", md5.c_str());
}

static void test_md5_wrong_length_rejected() {
  String short31 = "0123456789abcdef0123456789abcde";
  String long33 = "0123456789abcdef0123456789abcdef0";
  String empty = "";
  TEST_ASSERT_FALSE(normalizeOtaMd5(short31));
  TEST_ASSERT_FALSE(normalizeOtaMd5(long33));
  TEST_ASSERT_FALSE(normalizeOtaMd5(empty));
}

static void test_md5_non_hex_rejected() {
  String bad = "0123456789abcdef0123456789abcdeg";
  TEST_ASSERT_FALSE(normalizeOtaMd5(bad));
}

// --- pre-flash gate ---------------------------------------------------------

static void test_gate_refuses_cross_origin_before_looking_at_md5() {
  String md5 = "not-a-digest";
  TEST_ASSERT_EQUAL((int)OtaGate::CrossOrigin,
                    (int)otaUploadGate(true, false, md5));
  TEST_ASSERT_EQUAL(403, otaGateHttpStatus(OtaGate::CrossOrigin));
}

static void test_gate_md5_missing_and_malformed_are_400() {
  String none;
  TEST_ASSERT_EQUAL((int)OtaGate::Md5Missing,
                    (int)otaUploadGate(false, false, none));
  String nonHex = "zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz";  // 32 chars, not hex
  TEST_ASSERT_EQUAL((int)OtaGate::Md5Malformed,
                    (int)otaUploadGate(false, false, nonHex));
  TEST_ASSERT_EQUAL(400, otaGateHttpStatus(OtaGate::Md5Missing));
  TEST_ASSERT_EQUAL(400, otaGateHttpStatus(OtaGate::Md5Malformed));
}

static void test_gate_pass_normalizes_the_digest() {
  String md5 = "D41D8CD98F00B204E9800998ECF8427E";
  TEST_ASSERT_EQUAL((int)OtaGate::Pass, (int)otaUploadGate(false, false, md5));
  TEST_ASSERT_EQUAL_STRING("d41d8cd98f00b204e9800998ecf8427e", md5.c_str());
}

static void test_gate_unit_reflash_busy_is_409() {
  String md5 = "d41d8cd98f00b204e9800998ecf8427e";
  TEST_ASSERT_EQUAL((int)OtaGate::UnitReflashBusy,
                    (int)otaUploadGate(false, true, md5));
  TEST_ASSERT_EQUAL(409, otaGateHttpStatus(OtaGate::UnitReflashBusy));
}

// --- rejection slot ---------------------------------------------------------

// A rejection is answered once. Left set, the next POST without a file part
// would echo it instead of its own 400.
static void test_rejection_is_consumed_by_take() {
  OtaRejection r;
  TEST_ASSERT_FALSE(r.rejected());
  r.set(OtaGate::Md5Missing);
  TEST_ASSERT_TRUE(r.rejected());
  int status = 0;
  String reason;
  TEST_ASSERT_TRUE(r.take(status, reason));
  TEST_ASSERT_EQUAL(400, status);
  TEST_ASSERT_TRUE(reason.length() > 0);
  TEST_ASSERT_FALSE(r.rejected());
  TEST_ASSERT_FALSE(r.take(status, reason));
}

// --- completion verdict -----------------------------------------------------

static void test_completion_order() {
  TEST_ASSERT_EQUAL((int)OtaCompletion::Rejected,
                    (int)otaUploadCompletion(true, true, false, false));
  TEST_ASSERT_EQUAL((int)OtaCompletion::FlashError,
                    (int)otaUploadCompletion(false, true, true, true));
  TEST_ASSERT_EQUAL((int)OtaCompletion::Incomplete,
                    (int)otaUploadCompletion(false, false, true, false));
  TEST_ASSERT_EQUAL((int)OtaCompletion::Flashed,
                    (int)otaUploadCompletion(false, false, true, true));
}

// #347: a never-begun Update reports itself finished. Without a file part
// nothing was flashed, so the answer is never "flashed" (which reboots).
static void test_completion_without_a_file_part_never_reports_flashed() {
  TEST_ASSERT_EQUAL((int)OtaCompletion::NoFile,
                    (int)otaUploadCompletion(false, false, false, true));
}

// --- stall rule -------------------------------------------------------------

static void test_stall_after_the_timeout_and_across_millis_wrap() {
  TEST_ASSERT_FALSE(otaUploadStalled(1000, 1000 + OTA_STALL_TIMEOUT_MS));
  TEST_ASSERT_TRUE(otaUploadStalled(1000, 1001 + OTA_STALL_TIMEOUT_MS));
  TEST_ASSERT_FALSE(otaUploadStalled(0xFFFFFF00u, 0x00000100u));
  // A chunk stamped just after `now` was sampled is not 49 days of silence.
  TEST_ASSERT_FALSE(otaUploadStalled(5010, 5000));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_gate_refuses_cross_origin_before_looking_at_md5);
  RUN_TEST(test_gate_md5_missing_and_malformed_are_400);
  RUN_TEST(test_gate_pass_normalizes_the_digest);
  RUN_TEST(test_gate_unit_reflash_busy_is_409);
  RUN_TEST(test_rejection_is_consumed_by_take);
  RUN_TEST(test_completion_order);
  RUN_TEST(test_completion_without_a_file_part_never_reports_flashed);
  RUN_TEST(test_stall_after_the_timeout_and_across_millis_wrap);
  RUN_TEST(test_md5_lowercase_hex_accepted);
  RUN_TEST(test_md5_uppercase_normalized_in_place);
  RUN_TEST(test_md5_wrong_length_rejected);
  RUN_TEST(test_md5_non_hex_rejected);
  return UNITY_END();
}
