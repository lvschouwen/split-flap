#include "frame_reader.h"

namespace splitflap::frame {

Reader::Reader(uint8_t* scratch, size_t scratch_cap,
               uint8_t* payload, size_t payload_cap) noexcept
    : scratch_(scratch),
      scratch_cap_(scratch_cap),
      payload_(payload),
      payload_cap_(payload_cap),
      write_idx_(0),
      overflowed_(false),
      last_payload_len_(0) {}

void Reader::reset() noexcept {
  write_idx_ = 0;
  overflowed_ = false;
  last_payload_len_ = 0;
}

Reader::Event Reader::feed(uint8_t byte) noexcept {
  Event ev{Event::Kind::None, Error::Ok};

  if (byte != 0x00) {
    if (write_idx_ < scratch_cap_) {
      scratch_[write_idx_++] = byte;
    } else {
      overflowed_ = true;
    }
    return ev;
  }

  // Delimiter reached — try to decode whatever we have.
  if (overflowed_) {
    reset();
    ev.kind = Event::Kind::Dropped;
    ev.err  = Error::BufferTooSmall;
    return ev;
  }
  if (write_idx_ == 0) {
    // Extra delimiters between frames (or the very first one on power-up)
    // are benign. Stay quiet.
    return ev;
  }

  const auto res = decode(scratch_, write_idx_, payload_, payload_cap_);
  reset();
  if (res.err != Error::Ok) {
    ev.kind = Event::Kind::Dropped;
    ev.err  = res.err;
    return ev;
  }

  last_payload_len_ = res.payload_len;
  ev.kind = Event::Kind::Frame;
  return ev;
}

}  // namespace splitflap::frame
