// Stateful byte-to-frame reassembler for the S3<->H2 UART link.
//
// The wire is COBS-stuffed (see cobs_crc.h), so 0x00 is the sole frame
// delimiter. Reader buffers bytes up to the next 0x00, then calls
// splitflap::frame::decode on the accumulated bytes and reports either a
// valid payload or an error. The caller owns both buffers (encoded scratch
// and decoded payload) so storage is explicit — no dynamic allocation.
//
// Use: feed() one byte at a time from the UART RX path. When feed() returns
// kind == Frame, the payload is valid until the next feed() call.

#pragma once

#include <cstddef>
#include <cstdint>

#include "cobs_crc.h"

namespace splitflap::frame {

class Reader {
 public:
  struct Event {
    enum class Kind : uint8_t {
      None,     // still accumulating
      Frame,    // a valid payload was decoded; call Reader::payload()
      Dropped,  // frame terminated with an error; `err` describes
    };
    Kind kind;
    Error err;  // meaningful only when kind == Dropped
  };

  // `scratch` buffers COBS-stuffed bytes up to (but not including) the 0x00
  // delimiter. `scratch_cap` is the biggest frame the reader can accept —
  // larger frames get dropped with err=BufferTooSmall.
  // `payload` receives the decoded payload and must be at least scratch_cap
  // bytes long (decode writes through it during its scratch pass).
  Reader(uint8_t* scratch, size_t scratch_cap,
         uint8_t* payload, size_t payload_cap) noexcept;

  // Feed one received byte. On 0x00, tries to decode. Otherwise accumulates.
  Event feed(uint8_t byte) noexcept;

  // Reset in-progress state (used on UART errors, port reinit, etc.).
  void reset() noexcept;

  const uint8_t* payload() const noexcept { return payload_; }
  size_t payloadLen() const noexcept { return last_payload_len_; }

 private:
  uint8_t* scratch_;
  size_t   scratch_cap_;
  uint8_t* payload_;
  size_t   payload_cap_;
  size_t   write_idx_;
  bool     overflowed_;
  size_t   last_payload_len_;
};

}  // namespace splitflap::frame
