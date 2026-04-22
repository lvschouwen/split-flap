#include "protocol.h"

namespace splitflap::proto {

namespace {

size_t writeHeader(MsgType type, uint16_t seq,
                   uint8_t* out, size_t cap) noexcept {
  if (cap < kHeaderSize) return 0;
  out[0] = kVersion;
  out[1] = static_cast<uint8_t>(type);
  out[2] = static_cast<uint8_t>((seq >> 8) & 0xFF);
  out[3] = static_cast<uint8_t>(seq & 0xFF);
  return kHeaderSize;
}

}  // namespace

size_t encodePing(uint16_t seq, uint8_t* out, size_t cap) noexcept {
  return writeHeader(MsgType::Ping, seq, out, cap);
}

size_t encodePong(uint16_t seq, uint8_t* out, size_t cap) noexcept {
  return writeHeader(MsgType::Pong, seq, out, cap);
}

size_t encodeLog(uint16_t seq, const char* text, size_t text_len,
                 uint8_t* out, size_t cap) noexcept {
  if (cap < kHeaderSize + text_len) return 0;
  const size_t h = writeHeader(MsgType::Log, seq, out, cap);
  for (size_t i = 0; i < text_len; ++i) {
    out[h + i] = static_cast<uint8_t>(text[i]);
  }
  return h + text_len;
}

bool parse(const uint8_t* payload, size_t payload_len, Parsed* out) noexcept {
  if (out == nullptr) return false;
  if (payload_len < kHeaderSize) return false;
  if (payload[0] != kVersion) return false;

  const auto raw_type = payload[1];
  switch (raw_type) {
    case static_cast<uint8_t>(MsgType::Ping):
    case static_cast<uint8_t>(MsgType::Pong):
    case static_cast<uint8_t>(MsgType::Log):
      break;
    default:
      return false;
  }

  out->version = payload[0];
  out->type = static_cast<MsgType>(raw_type);
  out->seq = static_cast<uint16_t>((payload[2] << 8) | payload[3]);
  out->body = payload + kHeaderSize;
  out->body_len = payload_len - kHeaderSize;

  // Ping and Pong carry no body.
  if ((out->type == MsgType::Ping || out->type == MsgType::Pong) &&
      out->body_len != 0) {
    return false;
  }
  return true;
}

}  // namespace splitflap::proto
