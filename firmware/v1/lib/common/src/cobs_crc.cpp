#include "cobs_crc.h"

namespace splitflap::crc {

uint16_t crc16CcittFalse(const uint8_t* data, size_t len,
                         uint16_t seed) noexcept {
  uint16_t crc = seed;
  for (size_t i = 0; i < len; ++i) {
    crc ^= static_cast<uint16_t>(data[i]) << 8;
    for (int b = 0; b < 8; ++b) {
      crc = (crc & 0x8000u) ? static_cast<uint16_t>((crc << 1) ^ 0x1021u)
                            : static_cast<uint16_t>(crc << 1);
    }
  }
  return crc;
}

}  // namespace splitflap::crc

namespace splitflap::cobs {

size_t encode(const uint8_t* in, size_t in_len,
              uint8_t* out, size_t out_cap) noexcept {
  const size_t max_out = encodeBound(in_len);
  if (out_cap < max_out) return 0;

  size_t out_idx = 0;
  size_t code_idx = out_idx++;  // reserve first code byte slot
  uint8_t code = 0x01;

  for (size_t i = 0; i < in_len; ++i) {
    const uint8_t byte = in[i];
    if (byte == 0x00) {
      out[code_idx] = code;
      code_idx = out_idx++;
      code = 0x01;
    } else {
      out[out_idx++] = byte;
      ++code;
      if (code == 0xFF) {
        // Block full — emit the code and start a new block.
        out[code_idx] = code;
        code_idx = out_idx++;
        code = 0x01;
      }
    }
  }
  out[code_idx] = code;
  return out_idx;
}

DecodeResult decode(const uint8_t* in, size_t in_len,
                    uint8_t* out, size_t out_cap) noexcept {
  DecodeResult res{false, 0};
  if (in_len == 0) return res;

  size_t i = 0;
  size_t out_idx = 0;
  while (i < in_len) {
    const uint8_t code = in[i++];
    if (code == 0x00) return res;  // spurious zero inside the block

    const size_t run = code - 1u;
    if (i + run > in_len) return res;  // pointer walked past the end
    if (out_idx + run > out_cap) return res;

    for (size_t k = 0; k < run; ++k) {
      const uint8_t b = in[i++];
      if (b == 0x00) return res;  // zero byte inside the encoded payload
      out[out_idx++] = b;
    }

    if (i < in_len && code != 0xFF) {
      if (out_idx >= out_cap) return res;
      out[out_idx++] = 0x00;
    }
    // Last block's trailing zero is omitted — the frame delimiter stands in.
  }

  res.ok = true;
  res.out_len = out_idx;
  return res;
}

}  // namespace splitflap::cobs

namespace splitflap::frame {

size_t encode(const uint8_t* payload, size_t payload_len,
              uint8_t* out, size_t out_cap) noexcept {
  const size_t needed = encodeBound(payload_len);
  if (out_cap < needed) return 0;

  // Build the pre-COBS buffer: payload || CRC16-BE on a small scratch area.
  // We encode in-place by streaming into `out` through COBS after computing
  // CRC over the raw payload.
  const uint16_t c = splitflap::crc::crc16CcittFalse(payload, payload_len);

  // Compose [payload || crc_hi || crc_lo] in a scratch buffer.
  // Keep it stack-local and bounded by encodeBound minus COBS overhead.
  // Worst case here is payload_len + 2 bytes.
  // For embedded use we want to avoid dynamic alloc; cap the staging size.
  // Caller passed payload_len, so we trust it; use a VLA-free approach via
  // a second pass in encode (not possible with streaming COBS that needs
  // the full block upfront without a temp buffer). So: require out_cap
  // large enough to also hold the staging as a prefix area, then COBS
  // into the rest. Simpler: do a temp buffer stored at the end of `out`.

  // Place staging in the tail of `out` (it's large enough since out_cap >=
  // encodeBound(payload_len) which includes COBS overhead + delimiter).
  // staging goes at out + (out_cap - (payload_len + 2)).
  const size_t staging_len = payload_len + 2;
  uint8_t* staging = out + (out_cap - staging_len);
  for (size_t i = 0; i < payload_len; ++i) staging[i] = payload[i];
  staging[payload_len]     = static_cast<uint8_t>((c >> 8) & 0xFF);
  staging[payload_len + 1] = static_cast<uint8_t>(c & 0xFF);

  const size_t written = splitflap::cobs::encode(staging, staging_len,
                                                 out, out_cap - 1);
  if (written == 0) return 0;
  out[written] = 0x00;  // frame delimiter
  return written + 1;
}

DecodeResult decode(const uint8_t* frame_bytes, size_t frame_len,
                    uint8_t* out, size_t out_cap) noexcept {
  DecodeResult res{Error::Ok, 0};

  if (frame_len == 0) {
    res.err = Error::EmptyFrame;
    return res;
  }

  // Decode COBS into a scratch region. Worst case decoded size = frame_len - 1.
  // Write into the end of `out` to avoid a second buffer, then CRC-check and
  // copy the payload prefix down.
  // out_cap must be at least (frame_len - 1) bytes to hold the decoded staging.
  if (out_cap < frame_len) {
    res.err = Error::BufferTooSmall;
    return res;
  }

  // Decode directly into `out`. After decoding we have [payload || CRC_BE].
  const auto dec = splitflap::cobs::decode(frame_bytes, frame_len,
                                           out, out_cap);
  if (!dec.ok) {
    res.err = Error::InvalidCobs;
    return res;
  }
  if (dec.out_len < 2) {
    res.err = Error::ShortFrame;
    return res;
  }

  const size_t payload_len = dec.out_len - 2;
  const uint16_t got_hi = out[payload_len];
  const uint16_t got_lo = out[payload_len + 1];
  const uint16_t got = static_cast<uint16_t>((got_hi << 8) | got_lo);
  const uint16_t want = splitflap::crc::crc16CcittFalse(out, payload_len);
  if (got != want) {
    res.err = Error::BadCrc;
    return res;
  }

  res.payload_len = payload_len;
  return res;
}

}  // namespace splitflap::frame
