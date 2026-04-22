// COBS framing + CRC-16/CCITT-FALSE for the S3 <-> H2 UART link.
//
// Pure C++: no <Arduino.h>. Safe to include from ESP32 firmwares and from
// host-side native tests under firmware/lib/common/test/.
//
// Wire format for one frame:
//
//   [ COBS( payload || CRC16-BE(payload) ) ] 0x00
//
// where `||` is concatenation and the CRC is transmitted big-endian
// (high byte first). The trailing 0x00 is the only zero byte ever on the
// wire, so receivers can re-sync by scanning to the next 0x00.
//
// Layering: splitflap::crc provides the CRC primitive, splitflap::cobs
// provides raw COBS encode/decode (no CRC, no delimiter), and
// splitflap::frame stacks them into a verified payload codec.

#pragma once

#include <cstddef>
#include <cstdint>

namespace splitflap::crc {

// CRC-16/CCITT-FALSE: poly=0x1021, init=0xFFFF, no reflect, xorout=0x0000.
// Check value for the ASCII string "123456789" is 0x29B1.
uint16_t crc16CcittFalse(const uint8_t* data, size_t len,
                         uint16_t seed = 0xFFFF) noexcept;

}  // namespace splitflap::crc

namespace splitflap::cobs {

// Upper bound on the output size of cobs::encode for a given input size.
// COBS adds one overhead byte per block of 254 input bytes, plus one byte
// for the first block's code byte. We do NOT include a trailing delimiter
// here — frame::encode adds that separately.
constexpr size_t encodeBound(size_t in_len) noexcept {
  return in_len + (in_len / 254) + 1;
}

// Encodes `in[0..in_len)` into `out` using COBS. Returns bytes written.
// Does NOT append a 0x00 delimiter. Returns 0 if out_cap is too small.
size_t encode(const uint8_t* in, size_t in_len,
              uint8_t* out, size_t out_cap) noexcept;

struct DecodeResult {
  bool ok;
  size_t out_len;
};

// Decodes a COBS-encoded buffer (NOT including the trailing 0x00 delimiter).
// On success, `ok` is true and `out_len` is the decoded byte count.
// Rejects malformed inputs (pointer runs off the end, trailing zero inside
// the encoded block, empty input).
DecodeResult decode(const uint8_t* in, size_t in_len,
                    uint8_t* out, size_t out_cap) noexcept;

}  // namespace splitflap::cobs

namespace splitflap::frame {

enum class Error : uint8_t {
  Ok = 0,
  BufferTooSmall,   // output buffer cannot hold the result
  InvalidCobs,      // COBS pointer walked off the end
  ShortFrame,       // frame too small to contain a CRC
  BadCrc,           // decoded payload fails CRC check
  EmptyFrame,       // zero-byte frame
};

// Upper bound on the wire size of one frame including the trailing 0x00.
constexpr size_t encodeBound(size_t payload_len) noexcept {
  return splitflap::cobs::encodeBound(payload_len + 2) + 1;
}

// Encodes payload into a framed wire representation, terminated by 0x00.
// Returns total bytes written (including the terminator), or 0 if out_cap
// is insufficient.
size_t encode(const uint8_t* payload, size_t payload_len,
              uint8_t* out, size_t out_cap) noexcept;

struct DecodeResult {
  Error err;
  size_t payload_len;
};

// Decodes one frame. `frame_bytes[0..frame_len)` must be the COBS-stuffed
// bytes of the frame WITHOUT the trailing 0x00 delimiter (caller splits the
// stream on 0x00). On success, the verified payload is written to `out` and
// its length is reported in `payload_len`.
DecodeResult decode(const uint8_t* frame_bytes, size_t frame_len,
                    uint8_t* out, size_t out_cap) noexcept;

}  // namespace splitflap::frame
