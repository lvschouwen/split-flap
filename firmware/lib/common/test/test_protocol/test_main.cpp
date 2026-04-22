// Host-side tests for splitflap::proto (message encode/parse).

#include <unity.h>

#include <array>
#include <cstdint>
#include <cstring>

#include "protocol.h"

namespace p = splitflap::proto;

void setUp() {}
void tearDown() {}

// ---- header layout sanity -------------------------------------------------

void test_header_layout_ping() {
  std::array<uint8_t, 16> buf{};
  const size_t n = p::encodePing(0xABCD, buf.data(), buf.size());
  TEST_ASSERT_EQUAL_size_t(p::kHeaderSize, n);
  TEST_ASSERT_EQUAL_HEX8(p::kVersion, buf[0]);
  TEST_ASSERT_EQUAL_HEX8(static_cast<uint8_t>(p::MsgType::Ping), buf[1]);
  TEST_ASSERT_EQUAL_HEX8(0xAB, buf[2]);  // seq hi
  TEST_ASSERT_EQUAL_HEX8(0xCD, buf[3]);  // seq lo
}

void test_header_layout_pong() {
  std::array<uint8_t, 16> buf{};
  const size_t n = p::encodePong(0x0102, buf.data(), buf.size());
  TEST_ASSERT_EQUAL_size_t(p::kHeaderSize, n);
  TEST_ASSERT_EQUAL_HEX8(p::kVersion, buf[0]);
  TEST_ASSERT_EQUAL_HEX8(static_cast<uint8_t>(p::MsgType::Pong), buf[1]);
  TEST_ASSERT_EQUAL_HEX8(0x01, buf[2]);
  TEST_ASSERT_EQUAL_HEX8(0x02, buf[3]);
}

// ---- encode + parse round-trips ------------------------------------------

void test_ping_roundtrip() {
  std::array<uint8_t, 16> buf{};
  const size_t n = p::encodePing(7, buf.data(), buf.size());

  p::Parsed parsed{};
  TEST_ASSERT_TRUE(p::parse(buf.data(), n, &parsed));
  TEST_ASSERT_EQUAL_UINT8(p::kVersion, parsed.version);
  TEST_ASSERT_EQUAL(p::MsgType::Ping, parsed.type);
  TEST_ASSERT_EQUAL_UINT16(7, parsed.seq);
  TEST_ASSERT_EQUAL_size_t(0, parsed.body_len);
}

void test_pong_roundtrip() {
  std::array<uint8_t, 16> buf{};
  const size_t n = p::encodePong(0xFFFF, buf.data(), buf.size());

  p::Parsed parsed{};
  TEST_ASSERT_TRUE(p::parse(buf.data(), n, &parsed));
  TEST_ASSERT_EQUAL(p::MsgType::Pong, parsed.type);
  TEST_ASSERT_EQUAL_UINT16(0xFFFF, parsed.seq);
  TEST_ASSERT_EQUAL_size_t(0, parsed.body_len);
}

void test_log_roundtrip() {
  const char text[] = "heartbeat 42";
  const size_t text_len = sizeof(text) - 1;  // drop null terminator
  std::array<uint8_t, 64> buf{};
  const size_t n = p::encodeLog(3, text, text_len, buf.data(), buf.size());
  TEST_ASSERT_EQUAL_size_t(p::kHeaderSize + text_len, n);

  p::Parsed parsed{};
  TEST_ASSERT_TRUE(p::parse(buf.data(), n, &parsed));
  TEST_ASSERT_EQUAL(p::MsgType::Log, parsed.type);
  TEST_ASSERT_EQUAL_UINT16(3, parsed.seq);
  TEST_ASSERT_EQUAL_size_t(text_len, parsed.body_len);
  TEST_ASSERT_EQUAL_MEMORY(text, parsed.body, text_len);
}

void test_log_empty_body_is_valid() {
  std::array<uint8_t, 16> buf{};
  const size_t n = p::encodeLog(1, nullptr, 0, buf.data(), buf.size());
  TEST_ASSERT_EQUAL_size_t(p::kHeaderSize, n);

  p::Parsed parsed{};
  TEST_ASSERT_TRUE(p::parse(buf.data(), n, &parsed));
  TEST_ASSERT_EQUAL(p::MsgType::Log, parsed.type);
  TEST_ASSERT_EQUAL_size_t(0, parsed.body_len);
}

// ---- parse error paths ----------------------------------------------------

void test_parse_rejects_truncated_header() {
  const uint8_t buf[] = {p::kVersion, 0x01, 0x00};  // 3 bytes, need 4
  p::Parsed parsed{};
  TEST_ASSERT_FALSE(p::parse(buf, sizeof(buf), &parsed));
}

void test_parse_rejects_wrong_version() {
  const uint8_t buf[] = {0x99, 0x01, 0x00, 0x00};
  p::Parsed parsed{};
  TEST_ASSERT_FALSE(p::parse(buf, sizeof(buf), &parsed));
}

void test_parse_rejects_unknown_type() {
  const uint8_t buf[] = {p::kVersion, 0x7F, 0x00, 0x00};
  p::Parsed parsed{};
  TEST_ASSERT_FALSE(p::parse(buf, sizeof(buf), &parsed));
}

void test_parse_rejects_ping_with_body() {
  // Ping carries no body; any trailing bytes are a protocol violation.
  const uint8_t buf[] = {p::kVersion, static_cast<uint8_t>(p::MsgType::Ping),
                         0x00, 0x00, 0xAA};
  p::Parsed parsed{};
  TEST_ASSERT_FALSE(p::parse(buf, sizeof(buf), &parsed));
}

// ---- encode error paths ---------------------------------------------------

void test_encode_ping_rejects_tiny_buffer() {
  uint8_t tiny[3] = {};
  TEST_ASSERT_EQUAL_size_t(0, p::encodePing(1, tiny, sizeof(tiny)));
}

void test_encode_log_rejects_tiny_buffer() {
  const char text[] = "xyz";
  uint8_t tiny[5] = {};  // need 4 + 3 = 7
  TEST_ASSERT_EQUAL_size_t(0, p::encodeLog(0, text, 3, tiny, sizeof(tiny)));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_header_layout_ping);
  RUN_TEST(test_header_layout_pong);

  RUN_TEST(test_ping_roundtrip);
  RUN_TEST(test_pong_roundtrip);
  RUN_TEST(test_log_roundtrip);
  RUN_TEST(test_log_empty_body_is_valid);

  RUN_TEST(test_parse_rejects_truncated_header);
  RUN_TEST(test_parse_rejects_wrong_version);
  RUN_TEST(test_parse_rejects_unknown_type);
  RUN_TEST(test_parse_rejects_ping_with_body);

  RUN_TEST(test_encode_ping_rejects_tiny_buffer);
  RUN_TEST(test_encode_log_rejects_tiny_buffer);
  return UNITY_END();
}
