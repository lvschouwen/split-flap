// Host-side tests for splitflap::crc + splitflap::cobs + splitflap::frame.
//
// Run with `pio test -e native` from firmware/lib/common/. No Arduino
// dependencies — these layers are pure C++.

#include <unity.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include "cobs_crc.h"

using splitflap::cobs::decode;
using splitflap::cobs::encode;
using splitflap::cobs::encodeBound;
using splitflap::crc::crc16CcittFalse;
namespace fr = splitflap::frame;

void setUp() {}
void tearDown() {}

// ---- CRC-16/CCITT-FALSE --------------------------------------------------

void test_crc_check_value() {
  // The canonical check value for CRC-16/CCITT-FALSE is the CRC of the ASCII
  // string "123456789", which equals 0x29B1.
  const uint8_t data[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
  TEST_ASSERT_EQUAL_HEX16(0x29B1, crc16CcittFalse(data, sizeof(data)));
}

void test_crc_empty_input_returns_seed() {
  TEST_ASSERT_EQUAL_HEX16(0xFFFF, crc16CcittFalse(nullptr, 0));
}

void test_crc_single_byte() {
  const uint8_t data[] = {0xAB};
  const uint16_t c = crc16CcittFalse(data, 1);
  // Sanity: differs from seed, and a second call with a different byte differs.
  TEST_ASSERT_NOT_EQUAL(0xFFFF, c);
  const uint8_t other[] = {0xAC};
  TEST_ASSERT_NOT_EQUAL(c, crc16CcittFalse(other, 1));
}

// ---- COBS round-trip ------------------------------------------------------

static void roundTripCobs(const std::vector<uint8_t>& in) {
  std::vector<uint8_t> enc(encodeBound(in.size()));
  const size_t enc_len = encode(in.data(), in.size(), enc.data(), enc.size());
  TEST_ASSERT_GREATER_THAN(0, enc_len);

  // No zero bytes in encoded output (0x00 is reserved as frame delimiter).
  for (size_t i = 0; i < enc_len; ++i) {
    TEST_ASSERT_NOT_EQUAL(0x00, enc[i]);
  }

  std::vector<uint8_t> out(in.size() + 8, 0xCC);
  const auto res = decode(enc.data(), enc_len, out.data(), out.size());
  TEST_ASSERT_TRUE(res.ok);
  TEST_ASSERT_EQUAL_size_t(in.size(), res.out_len);
  if (!in.empty()) {
    TEST_ASSERT_EQUAL_MEMORY(in.data(), out.data(), in.size());
  }
}

void test_cobs_roundtrip_empty() {
  roundTripCobs({});
}

void test_cobs_roundtrip_single_nonzero_byte() {
  roundTripCobs({0xAA});
}

void test_cobs_roundtrip_single_zero_byte() {
  roundTripCobs({0x00});
}

void test_cobs_roundtrip_all_zeros() {
  roundTripCobs(std::vector<uint8_t>(32, 0x00));
}

void test_cobs_roundtrip_no_zeros() {
  std::vector<uint8_t> v(200);
  for (size_t i = 0; i < v.size(); ++i) v[i] = static_cast<uint8_t>((i % 255) + 1);
  roundTripCobs(v);
}

void test_cobs_roundtrip_long_payload_crosses_254_boundary() {
  // 254 non-zero bytes force a block-full case in the encoder.
  std::vector<uint8_t> v(600);
  for (size_t i = 0; i < v.size(); ++i) {
    v[i] = static_cast<uint8_t>(((i * 7) % 255) + 1);
  }
  roundTripCobs(v);
}

void test_cobs_roundtrip_mixed_with_zeros() {
  std::vector<uint8_t> v = {0x11, 0x00, 0x22, 0x33, 0x00, 0x00, 0x44};
  roundTripCobs(v);
}

// ---- COBS decode error cases ---------------------------------------------

void test_cobs_decode_rejects_empty() {
  uint8_t out[4] = {};
  const auto res = decode(nullptr, 0, out, sizeof(out));
  TEST_ASSERT_FALSE(res.ok);
}

void test_cobs_decode_rejects_zero_code() {
  const uint8_t enc[] = {0x00};  // code byte = 0 is illegal mid-frame
  uint8_t out[4] = {};
  const auto res = decode(enc, sizeof(enc), out, sizeof(out));
  TEST_ASSERT_FALSE(res.ok);
}

void test_cobs_decode_rejects_pointer_past_end() {
  // Code byte says "5 more bytes follow" but buffer is only 3 bytes total.
  const uint8_t enc[] = {0x05, 0x01, 0x02};
  uint8_t out[8] = {};
  const auto res = decode(enc, sizeof(enc), out, sizeof(out));
  TEST_ASSERT_FALSE(res.ok);
}

void test_cobs_decode_rejects_zero_in_block() {
  // Inside a block, a literal 0x00 is a framing violation.
  const uint8_t enc[] = {0x04, 0x01, 0x00, 0x02};
  uint8_t out[8] = {};
  const auto res = decode(enc, sizeof(enc), out, sizeof(out));
  TEST_ASSERT_FALSE(res.ok);
}

// ---- frame round-trip -----------------------------------------------------

void test_frame_roundtrip_typical() {
  const uint8_t payload[] = {'h', 'e', 'l', 'l', 'o'};
  std::array<uint8_t, 64> wire{};
  const size_t wire_len = fr::encode(payload, sizeof(payload),
                                     wire.data(), wire.size());
  TEST_ASSERT_GREATER_THAN(sizeof(payload), wire_len);
  TEST_ASSERT_EQUAL(0x00, wire[wire_len - 1]);  // trailing delimiter

  std::array<uint8_t, 64> out{};
  const auto res = fr::decode(wire.data(), wire_len - 1,
                              out.data(), out.size());
  TEST_ASSERT_EQUAL(static_cast<int>(fr::Error::Ok), static_cast<int>(res.err));
  TEST_ASSERT_EQUAL_size_t(sizeof(payload), res.payload_len);
  TEST_ASSERT_EQUAL_MEMORY(payload, out.data(), sizeof(payload));
}

void test_frame_roundtrip_empty_payload() {
  std::array<uint8_t, 16> wire{};
  const size_t wire_len = fr::encode(nullptr, 0, wire.data(), wire.size());
  TEST_ASSERT_GREATER_THAN(0, wire_len);
  TEST_ASSERT_EQUAL(0x00, wire[wire_len - 1]);

  std::array<uint8_t, 16> out{};
  const auto res = fr::decode(wire.data(), wire_len - 1,
                              out.data(), out.size());
  TEST_ASSERT_EQUAL(static_cast<int>(fr::Error::Ok), static_cast<int>(res.err));
  TEST_ASSERT_EQUAL_size_t(0, res.payload_len);
}

void test_frame_roundtrip_payload_with_zeros() {
  const uint8_t payload[] = {0x00, 0x01, 0x00, 0xFF, 0x00};
  std::array<uint8_t, 32> wire{};
  const size_t wire_len = fr::encode(payload, sizeof(payload),
                                     wire.data(), wire.size());
  TEST_ASSERT_GREATER_THAN(0, wire_len);

  std::array<uint8_t, 32> out{};
  const auto res = fr::decode(wire.data(), wire_len - 1,
                              out.data(), out.size());
  TEST_ASSERT_EQUAL(static_cast<int>(fr::Error::Ok), static_cast<int>(res.err));
  TEST_ASSERT_EQUAL_size_t(sizeof(payload), res.payload_len);
  TEST_ASSERT_EQUAL_MEMORY(payload, out.data(), sizeof(payload));
}

void test_frame_decode_bit_flip_is_bad_crc() {
  const uint8_t payload[] = {'a', 'b', 'c', 'd'};
  std::array<uint8_t, 32> wire{};
  const size_t wire_len = fr::encode(payload, sizeof(payload),
                                     wire.data(), wire.size());

  // Flip a bit in the middle of the COBS-encoded payload.
  wire[wire_len / 2] ^= 0x01;

  std::array<uint8_t, 32> out{};
  const auto res = fr::decode(wire.data(), wire_len - 1,
                              out.data(), out.size());
  // Any bit flip either corrupts COBS structure or CRC; both are rejections.
  TEST_ASSERT_TRUE(res.err == fr::Error::BadCrc ||
                   res.err == fr::Error::InvalidCobs);
}

void test_frame_decode_empty_frame() {
  std::array<uint8_t, 8> out{};
  const auto res = fr::decode(nullptr, 0, out.data(), out.size());
  TEST_ASSERT_EQUAL(static_cast<int>(fr::Error::EmptyFrame),
                    static_cast<int>(res.err));
}

void test_frame_decode_short_frame() {
  // One byte of COBS decoding to a single byte — not enough for a 2-byte CRC.
  const uint8_t wire[] = {0x02, 0xAA};  // COBS(0xAA) = 0x02, 0xAA
  std::array<uint8_t, 8> out{};
  const auto res = fr::decode(wire, sizeof(wire), out.data(), out.size());
  TEST_ASSERT_EQUAL(static_cast<int>(fr::Error::ShortFrame),
                    static_cast<int>(res.err));
}

void test_frame_encode_rejects_small_buffer() {
  const uint8_t payload[] = {'x', 'y'};
  uint8_t tiny[2] = {};
  const size_t n = fr::encode(payload, sizeof(payload), tiny, sizeof(tiny));
  TEST_ASSERT_EQUAL_size_t(0, n);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_crc_check_value);
  RUN_TEST(test_crc_empty_input_returns_seed);
  RUN_TEST(test_crc_single_byte);

  RUN_TEST(test_cobs_roundtrip_empty);
  RUN_TEST(test_cobs_roundtrip_single_nonzero_byte);
  RUN_TEST(test_cobs_roundtrip_single_zero_byte);
  RUN_TEST(test_cobs_roundtrip_all_zeros);
  RUN_TEST(test_cobs_roundtrip_no_zeros);
  RUN_TEST(test_cobs_roundtrip_long_payload_crosses_254_boundary);
  RUN_TEST(test_cobs_roundtrip_mixed_with_zeros);

  RUN_TEST(test_cobs_decode_rejects_empty);
  RUN_TEST(test_cobs_decode_rejects_zero_code);
  RUN_TEST(test_cobs_decode_rejects_pointer_past_end);
  RUN_TEST(test_cobs_decode_rejects_zero_in_block);

  RUN_TEST(test_frame_roundtrip_typical);
  RUN_TEST(test_frame_roundtrip_empty_payload);
  RUN_TEST(test_frame_roundtrip_payload_with_zeros);
  RUN_TEST(test_frame_decode_bit_flip_is_bad_crc);
  RUN_TEST(test_frame_decode_empty_frame);
  RUN_TEST(test_frame_decode_short_frame);
  RUN_TEST(test_frame_encode_rejects_small_buffer);
  return UNITY_END();
}
