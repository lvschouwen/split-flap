// Host-side unit tests for RescueSlotRecord.h (#200) — the parse-only copy
// of Master's SlotRecord.h. Rescue shares nothing compiled with Master, so
// the record format is wire-contract-like: these tests pin the same
// acceptance rules Master's test_slot_record pins on the writer side.

#include <ArduinoFake.h>
#include <unity.h>

#include "../../RescueSlotRecord.h"

void setUp() {}
void tearDown() {}

static const char* SHA_HEX =
    "000102030405060708090a0b0c0d0e0f10a0b1c2d3e4f5ffdeadbeef11223344";

static void test_parse_valid_record() {
  String rec = String("1|7|") + SHA_HEX + "|83aa664-dirty";
  SlotRecord r = parseSlotRecord(rec.c_str());
  TEST_ASSERT_TRUE(r.ok);
  TEST_ASSERT_EQUAL_UINT32(7, r.seq);
  TEST_ASSERT_EQUAL_STRING(SHA_HEX, r.sha);
  TEST_ASSERT_EQUAL_STRING("83aa664-dirty", r.rev);
}

static void test_parse_max_seq() {
  String rec = String("1|4294967295|") + SHA_HEX + "|rev";
  SlotRecord r = parseSlotRecord(rec.c_str());
  TEST_ASSERT_TRUE(r.ok);
  TEST_ASSERT_EQUAL_UINT32(4294967295u, r.seq);
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
  String overflow = String("1|4294967296|") + SHA_HEX + "|rev";
  TEST_ASSERT_FALSE(parseSlotRecord(overflow.c_str()).ok);
}

static void test_parse_rejects_bad_sha() {
  TEST_ASSERT_FALSE(parseSlotRecord("1|7|deadbeef|rev").ok);  // too short
  String upper = String("1|7|") + SHA_HEX + "|rev";
  upper.setCharAt(4, 'A');  // Master writes lowercase only
  TEST_ASSERT_FALSE(parseSlotRecord(upper.c_str()).ok);
}

static void test_parse_rejects_bad_rev() {
  String empty = String("1|7|") + SHA_HEX + "|";
  TEST_ASSERT_FALSE(parseSlotRecord(empty.c_str()).ok);
  String quote = String("1|7|") + SHA_HEX + "|re\"v";  // must be JSON-safe
  TEST_ASSERT_FALSE(parseSlotRecord(quote.c_str()).ok);
  String tooLong = String("1|7|") + SHA_HEX +
                   "|012345678901234567890123456789012";  // 33 chars
  TEST_ASSERT_FALSE(parseSlotRecord(tooLong.c_str()).ok);
}

static void test_parse_rejects_missing_fields() {
  TEST_ASSERT_FALSE(parseSlotRecord("1|7").ok);
  String noRevSep = String("1|7|") + SHA_HEX;
  TEST_ASSERT_FALSE(parseSlotRecord(noRevSep.c_str()).ok);
}

static const uint8_t SHA_BYTES[32] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a,
    0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0xa0, 0xb1, 0xc2, 0xd3, 0xe4,
    0xf5, 0xff, 0xde, 0xad, 0xbe, 0xef, 0x11, 0x22, 0x33, 0x44};

static void test_sha_matches_same_bytes() {
  SlotRecord r = parseSlotRecord((String("1|7|") + SHA_HEX + "|rev").c_str());
  TEST_ASSERT_TRUE(slotRecordShaMatches(r, SHA_BYTES));
}

static void test_sha_rejects_different_bytes() {
  SlotRecord r = parseSlotRecord((String("1|7|") + SHA_HEX + "|rev").c_str());
  uint8_t other[32];
  memcpy(other, SHA_BYTES, 32);
  other[0] ^= 0x10;
  TEST_ASSERT_FALSE(slotRecordShaMatches(r, other));
}

static void test_sha_rejects_invalid_record_or_null() {
  SlotRecord bad;  // !ok
  TEST_ASSERT_FALSE(slotRecordShaMatches(bad, SHA_BYTES));
  SlotRecord r = parseSlotRecord((String("1|7|") + SHA_HEX + "|rev").c_str());
  TEST_ASSERT_FALSE(slotRecordShaMatches(r, nullptr));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_parse_valid_record);
  RUN_TEST(test_parse_max_seq);
  RUN_TEST(test_parse_rejects_null_and_empty);
  RUN_TEST(test_parse_rejects_unknown_version);
  RUN_TEST(test_parse_rejects_bad_seq);
  RUN_TEST(test_parse_rejects_bad_sha);
  RUN_TEST(test_parse_rejects_bad_rev);
  RUN_TEST(test_parse_rejects_missing_fields);
  RUN_TEST(test_sha_matches_same_bytes);
  RUN_TEST(test_sha_rejects_different_bytes);
  RUN_TEST(test_sha_rejects_invalid_record_or_null);
  return UNITY_END();
}
