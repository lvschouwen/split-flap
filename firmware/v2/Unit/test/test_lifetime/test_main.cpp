// Host-side unit tests for the SFP_CMD_GET_LIFETIME wire format in
// UnitLifetime.h (#406/#407) — the read path for the lifetime health fields
// the day-0 EEPROM layout added. Without it the fields would be write-only
// until another 21-unit reflash, which is precisely what epic #407 exists to
// avoid.

#include <unity.h>
#include <stdint.h>
#include <string.h>
#include "UnitLifetime.h"

void setUp() {}
void tearDown() {}

static UnitLifetimeFacts sample() {
  UnitLifetimeFacts f;
  f.layoutVersion = 1;
  f.homeFailedCount = 11;
  f.featureGates = 0x02;
  f.stepExcessLifetimeMax = 4001;
  f.selfTestFirstHallWindow = 46;
  f.selfTestFirstStepsPerRev = 2038;
  f.selfTestLastHallWindow = 12;
  f.selfTestLastStepsPerRev = 2044;
  return f;
}

// --- round trip -----------------------------------------------------------

static void test_roundtrip_all_fields() {
  uint8_t buf[LIFETIME_REPLY_LEN];
  lifetimeEncodeReply(sample(), buf);

  UnitLifetimeFacts out;
  TEST_ASSERT_TRUE(lifetimeReadbackValid(buf, LIFETIME_REPLY_LEN, out));
  TEST_ASSERT_EQUAL_UINT8(1, out.layoutVersion);
  TEST_ASSERT_EQUAL_UINT8(11, out.homeFailedCount);
  TEST_ASSERT_EQUAL_UINT8(0x02, out.featureGates);
  TEST_ASSERT_EQUAL_UINT16(4001, out.stepExcessLifetimeMax);
  TEST_ASSERT_EQUAL_UINT16(46, out.selfTestFirstHallWindow);
  TEST_ASSERT_EQUAL_UINT16(2038, out.selfTestFirstStepsPerRev);
  TEST_ASSERT_EQUAL_UINT16(12, out.selfTestLastHallWindow);
  TEST_ASSERT_EQUAL_UINT16(2044, out.selfTestLastStepsPerRev);
}

static void test_multibyte_fields_are_little_endian() {
  UnitLifetimeFacts f;
  f.stepExcessLifetimeMax = 0x0201;
  uint8_t buf[LIFETIME_REPLY_LEN];
  lifetimeEncodeReply(f, buf);
  TEST_ASSERT_EQUAL_UINT8(0x01, buf[4]);
  TEST_ASSERT_EQUAL_UINT8(0x02, buf[5]);
}

// --- mixed-firmware safety ------------------------------------------------
// MANDATORY for this epic: during the reflash the wall runs both firmwares.
// A unit that predates the opcode answers with its 1-byte rotation status
// plus whatever the bus pads with. Every one of those shapes must be
// REJECTED, never decoded into a phantom reading.

static void test_short_reply_from_an_old_unit_is_rejected() {
  uint8_t buf[LIFETIME_REPLY_LEN] = {0};
  lifetimeEncodeReply(sample(), buf);
  UnitLifetimeFacts out;
  // Correct bytes, but the transaction returned fewer of them.
  TEST_ASSERT_FALSE(lifetimeReadbackValid(buf, LIFETIME_REPLY_LEN - 1, out));
  TEST_ASSERT_FALSE(lifetimeReadbackValid(buf, 1, out));
  TEST_ASSERT_FALSE(lifetimeReadbackValid(buf, 0, out));
}

static void test_all_ff_padding_is_rejected() {
  uint8_t buf[LIFETIME_REPLY_LEN];
  memset(buf, 0xFF, sizeof(buf));
  UnitLifetimeFacts out;
  TEST_ASSERT_FALSE(lifetimeReadbackValid(buf, LIFETIME_REPLY_LEN, out));
}

static void test_all_zero_padding_is_rejected() {
  uint8_t buf[LIFETIME_REPLY_LEN] = {0};
  UnitLifetimeFacts out;
  TEST_ASSERT_FALSE(lifetimeReadbackValid(buf, LIFETIME_REPLY_LEN, out));
}

static void test_repeated_status_byte_padding_is_rejected() {
  // An idle unit answers an unknown opcode with currentlyrotating == 0 or 1.
  for (uint8_t fill = 0; fill < 2; fill++) {
    uint8_t buf[LIFETIME_REPLY_LEN];
    memset(buf, fill, sizeof(buf));
    UnitLifetimeFacts out;
    TEST_ASSERT_FALSE(lifetimeReadbackValid(buf, LIFETIME_REPLY_LEN, out));
  }
}

static void test_rejection_leaves_the_output_untouched() {
  // The master folds this into UnitFacts; a rejected read must not have
  // half-written it first.
  uint8_t buf[LIFETIME_REPLY_LEN];
  memset(buf, 0xFF, sizeof(buf));
  UnitLifetimeFacts out;
  out.homeFailedCount = 7;
  TEST_ASSERT_FALSE(lifetimeReadbackValid(buf, LIFETIME_REPLY_LEN, out));
  TEST_ASSERT_EQUAL_UINT8(7, out.homeFailedCount);
}

// --- deliberate corruption ------------------------------------------------

static void test_every_single_byte_flip_is_caught() {
  // One flipped bit anywhere in the payload must fail the checksum. This is
  // the property that makes a future 400 kHz attempt measurable rather than
  // guesswork (#383): corruption surfaces as rejection, not as a silent
  // wrong number.
  for (uint8_t i = 0; i < LIFETIME_REPLY_LEN; i++) {
    uint8_t buf[LIFETIME_REPLY_LEN];
    lifetimeEncodeReply(sample(), buf);
    buf[i] ^= 0x01;
    UnitLifetimeFacts out;
    TEST_ASSERT_FALSE(lifetimeReadbackValid(buf, LIFETIME_REPLY_LEN, out));
  }
}

static void test_zero_valued_packet_still_checksums_away_from_padding() {
  // A genuinely fresh unit reports all zeros. Its checksum must be the bare
  // mask, so the valid all-zero packet is distinguishable from a 0x00-padded
  // reply that carries no checksum at all.
  UnitLifetimeFacts fresh;
  uint8_t buf[LIFETIME_REPLY_LEN];
  lifetimeEncodeReply(fresh, buf);
  TEST_ASSERT_EQUAL_UINT8(LIFETIME_REPLY_CHECKSUM_MASK, buf[LIFETIME_REPLY_LEN - 1]);
  UnitLifetimeFacts out;
  TEST_ASSERT_TRUE(lifetimeReadbackValid(buf, LIFETIME_REPLY_LEN, out));
}

static void test_checksum_mask_is_distinct_from_the_other_replies() {
  // Distinct per opcode so a reply decoded against the wrong opcode's
  // validator cannot pass.
  TEST_ASSERT_TRUE(LIFETIME_REPLY_CHECKSUM_MASK != 0xA5);  // odometer
  TEST_ASSERT_TRUE(LIFETIME_REPLY_CHECKSUM_MASK != 0xB7);  // diag
  TEST_ASSERT_TRUE(LIFETIME_REPLY_CHECKSUM_MASK != 0x5C);  // self-test
  TEST_ASSERT_TRUE(LIFETIME_REPLY_CHECKSUM_MASK != 0x3C);  // vitals
  TEST_ASSERT_TRUE(LIFETIME_REPLY_CHECKSUM_MASK != 0x93);  // ext-diag
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_roundtrip_all_fields);
  RUN_TEST(test_multibyte_fields_are_little_endian);
  RUN_TEST(test_short_reply_from_an_old_unit_is_rejected);
  RUN_TEST(test_all_ff_padding_is_rejected);
  RUN_TEST(test_all_zero_padding_is_rejected);
  RUN_TEST(test_repeated_status_byte_padding_is_rejected);
  RUN_TEST(test_rejection_leaves_the_output_untouched);
  RUN_TEST(test_every_single_byte_flip_is_caught);
  RUN_TEST(test_zero_valued_packet_still_checksums_away_from_padding);
  RUN_TEST(test_checksum_mask_is_distinct_from_the_other_replies);
  UNITY_END();
  return 0;
}
