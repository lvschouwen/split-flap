// S3 <-> H2 UART protocol message types.
//
// Message payloads are built here, then handed to splitflap::frame::encode
// (cobs_crc.h) for CRC + COBS + delimiter.
//
// Wire header (4 bytes):
//   [ version:u8 ][ type:u8 ][ seq_hi:u8 ][ seq_lo:u8 ]
//
// Messages:
//   PING — header only (4 bytes).
//   PONG — header only (4 bytes); echoes the PING's seq.
//   LOG  — header + text bytes (no null terminator), H2 -> S3.
//
// The version byte is bumped whenever the message layout changes
// incompatibly. Parsers reject unknown versions.

#pragma once

#include <cstddef>
#include <cstdint>

namespace splitflap::proto {

inline constexpr uint8_t kVersion = 0x01;
inline constexpr size_t kHeaderSize = 4;

enum class MsgType : uint8_t {
  Ping = 0x01,
  Pong = 0x02,
  Log  = 0x03,
};

// Encodes a PING message into `out`. Returns bytes written, or 0 on
// insufficient capacity.
size_t encodePing(uint16_t seq, uint8_t* out, size_t cap) noexcept;

// Encodes a PONG message into `out`. Returns bytes written, or 0 on
// insufficient capacity.
size_t encodePong(uint16_t seq, uint8_t* out, size_t cap) noexcept;

// Encodes a LOG message into `out`. Returns bytes written, or 0 on
// insufficient capacity.
size_t encodeLog(uint16_t seq, const char* text, size_t text_len,
                 uint8_t* out, size_t cap) noexcept;

struct Parsed {
  uint8_t version;
  MsgType type;
  uint16_t seq;
  const uint8_t* body;
  size_t body_len;
};

// Parses a payload previously extracted by splitflap::frame::decode.
// Returns true on success. Rejects wrong version, truncated header, and
// unknown message types.
bool parse(const uint8_t* payload, size_t payload_len, Parsed* out) noexcept;

}  // namespace splitflap::proto
