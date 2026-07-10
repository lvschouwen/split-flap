// Host-side unit tests for SlotRecord.h (#200) — the NVS slot-record format
// Master writes on OTA health-confirm and Rescue ranks /rescue/exit by.
// Rescue parses these records from a separately compiled image, so the
// format is wire-contract-like: versioned, strictly validated, and the
// Rescue copy (RescueSlotRecord.h) must stay behaviorally identical.

#include <ArduinoFake.h>
#include <unity.h>

#include "../../SlotRecord.h"

void setUp() {}
void tearDown() {}

static const uint8_t SHA_BYTES[32] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a,
    0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0xa0, 0xb1, 0xc2, 0xd3, 0xe4,
    0xf5, 0xff, 0xde, 0xad, 0xbe, 0xef, 0x11, 0x22, 0x33, 0x44};
static const char* SHA_HEX =
    "000102030405060708090a0b0c0d0e0f10a0b1c2d3e4f5ffdeadbeef11223344";

// --- formatSlotRecord ---------------------------------------------------------

static void test_format_produces_versioned_record() {
  char buf[SLOT_RECORD_BUF_LEN];
  TEST_ASSERT_TRUE(formatSlotRecord(buf, sizeof(buf), 7, SHA_BYTES, "83aa664"));
  String expected = String("1|7|") + SHA_HEX + "|83aa664";
  TEST_ASSERT_EQUAL_STRING(expected.c_str(), buf);
}

static void test_format_rejects_short_buffer() {
  char buf[16];
  TEST_ASSERT_FALSE(formatSlotRecord(buf, sizeof(buf), 7, SHA_BYTES, "83aa664"));
}

static void test_format_truncates_long_rev() {
  // rev is display-only; longer than 32 chars is clamped, not an error.
  char buf[SLOT_RECORD_BUF_LEN];
  const char* longRev = "0123456789012345678901234567890123456789";
  TEST_ASSERT_TRUE(formatSlotRecord(buf, sizeof(buf), 1, SHA_BYTES, longRev));
  SlotRecord r = parseSlotRecord(buf);
  TEST_ASSERT_TRUE(r.ok);
  TEST_ASSERT_EQUAL_STRING("01234567890123456789012345678901", r.rev);
}

// --- parseSlotRecord ----------------------------------------------------------

static void test_parse_roundtrip() {
  char buf[SLOT_RECORD_BUF_LEN];
  TEST_ASSERT_TRUE(
      formatSlotRecord(buf, sizeof(buf), 4294967295u, SHA_BYTES, "abc-dirty"));
  SlotRecord r = parseSlotRecord(buf);
  TEST_ASSERT_TRUE(r.ok);
  TEST_ASSERT_EQUAL_UINT32(4294967295u, r.seq);
  TEST_ASSERT_EQUAL_STRING(SHA_HEX, r.sha);
  TEST_ASSERT_EQUAL_STRING("abc-dirty", r.rev);
}

static void test_parse_rejects_null_and_empty() {
  TEST_ASSERT_FALSE(parseSlotRecord(nullptr).ok);
  TEST_ASSERT_FALSE(parseSlotRecord("").ok);
}

static void test_parse_rejects_unknown_version() {
  String rec = String("2|7|") + SHA_HEX + "|rev";
  TEST_ASSERT_FALSE(parseSlotRecord(rec.c_str()).ok);
}

static void test_parse_rejects_bad_seq() {
  String noDigits = String("1||") + SHA_HEX + "|rev";
  TEST_ASSERT_FALSE(parseSlotRecord(noDigits.c_str()).ok);
  String alpha = String("1|x7|") + SHA_HEX + "|rev";
  TEST_ASSERT_FALSE(parseSlotRecord(alpha.c_str()).ok);
  String overflow = String("1|4294967296|") + SHA_HEX + "|rev";
  TEST_ASSERT_FALSE(parseSlotRecord(overflow.c_str()).ok);
}

static void test_parse_rejects_bad_sha() {
  TEST_ASSERT_FALSE(parseSlotRecord("1|7|deadbeef|rev").ok);  // too short
  String upper = String("1|7|") + SHA_HEX + "|rev";
  upper.setCharAt(4 + 0, 'A');  // uppercase hex is not what Master writes
  TEST_ASSERT_FALSE(parseSlotRecord(upper.c_str()).ok);
  String nonHex = String("1|7|") + SHA_HEX + "|rev";
  nonHex.setCharAt(4 + 0, 'g');
  TEST_ASSERT_FALSE(parseSlotRecord(nonHex.c_str()).ok);
}

static void test_parse_rejects_bad_rev() {
  String empty = String("1|7|") + SHA_HEX + "|";
  TEST_ASSERT_FALSE(parseSlotRecord(empty.c_str()).ok);
  String tooLong = String("1|7|") + SHA_HEX +
                   "|012345678901234567890123456789012";  // 33 chars
  TEST_ASSERT_FALSE(parseSlotRecord(tooLong.c_str()).ok);
  String quote = String("1|7|") + SHA_HEX + "|re\"v";  // must be JSON-safe
  TEST_ASSERT_FALSE(parseSlotRecord(quote.c_str()).ok);
}

static void test_parse_rejects_missing_fields() {
  TEST_ASSERT_FALSE(parseSlotRecord("1|7").ok);
  String noRevSep = String("1|7|") + SHA_HEX;
  TEST_ASSERT_FALSE(parseSlotRecord(noRevSep.c_str()).ok);
}

// --- slotRecordShaMatches -----------------------------------------------------

static void test_sha_matches_same_bytes() {
  SlotRecord r = parseSlotRecord((String("1|7|") + SHA_HEX + "|rev").c_str());
  TEST_ASSERT_TRUE(slotRecordShaMatches(r, SHA_BYTES));
}

static void test_sha_rejects_different_bytes() {
  SlotRecord r = parseSlotRecord((String("1|7|") + SHA_HEX + "|rev").c_str());
  uint8_t other[32];
  memcpy(other, SHA_BYTES, 32);
  other[31] ^= 0x01;
  TEST_ASSERT_FALSE(slotRecordShaMatches(r, other));
}

static void test_sha_rejects_invalid_record_or_null() {
  SlotRecord bad;  // !ok
  TEST_ASSERT_FALSE(slotRecordShaMatches(bad, SHA_BYTES));
  SlotRecord r = parseSlotRecord((String("1|7|") + SHA_HEX + "|rev").c_str());
  TEST_ASSERT_FALSE(slotRecordShaMatches(r, nullptr));
}

// --- nextSlotRecordSeq --------------------------------------------------------

static void test_next_seq_starts_at_one() {
  SlotRecord none;  // default: !ok
  TEST_ASSERT_EQUAL_UINT32(1, nextSlotRecordSeq(none, none));
}

static void test_next_seq_is_max_plus_one() {
  SlotRecord a = parseSlotRecord(
      (String("1|3|") + SHA_HEX + "|rev").c_str());
  SlotRecord b = parseSlotRecord(
      (String("1|9|") + SHA_HEX + "|rev").c_str());
  TEST_ASSERT_EQUAL_UINT32(10, nextSlotRecordSeq(a, b));
  TEST_ASSERT_EQUAL_UINT32(10, nextSlotRecordSeq(b, a));
  SlotRecord none;
  TEST_ASSERT_EQUAL_UINT32(4, nextSlotRecordSeq(a, none));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_format_produces_versioned_record);
  RUN_TEST(test_format_rejects_short_buffer);
  RUN_TEST(test_format_truncates_long_rev);
  RUN_TEST(test_parse_roundtrip);
  RUN_TEST(test_parse_rejects_null_and_empty);
  RUN_TEST(test_parse_rejects_unknown_version);
  RUN_TEST(test_parse_rejects_bad_seq);
  RUN_TEST(test_parse_rejects_bad_sha);
  RUN_TEST(test_parse_rejects_bad_rev);
  RUN_TEST(test_parse_rejects_missing_fields);
  RUN_TEST(test_sha_matches_same_bytes);
  RUN_TEST(test_sha_rejects_different_bytes);
  RUN_TEST(test_sha_rejects_invalid_record_or_null);
  RUN_TEST(test_next_seq_starts_at_one);
  RUN_TEST(test_next_seq_is_max_plus_one);
  return UNITY_END();
}
