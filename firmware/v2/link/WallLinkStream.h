// WallLinkStream.h — puts wall_link.proto messages on a byte stream and takes
// them off again. Encoding and decoding are nanopb's; what is here is the
// part nanopb leaves to the application: collecting bytes from a socket that
// is polled from loop() until one whole length-delimited message has arrived.
//
// Lives outside shared/ on purpose: no unit compiles it, so an edit here must
// not move the unit source head.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <pb_decode.h>
#include <pb_encode.h>

#include "wall_link.pb.h"

constexpr uint32_t WALL_LINK_PROTOCOL = 1;
constexpr uint16_t WALL_LINK_PORT = 7411;

// A length prefix is a varint of at most 2 bytes for our message sizes; 3
// leaves room without ever accepting an absurd length.
constexpr size_t WL_PREFIX_MAX = 3;

// Encodes one message with its length prefix; returns the bytes written, 0
// when it does not fit or a field is out of range.
inline size_t wlEncode(uint8_t* out, size_t cap, const pb_msgdesc_t* fields, const void* msg) {
  pb_ostream_t s = pb_ostream_from_buffer(out, cap);
  return pb_encode_ex(&s, fields, msg, PB_ENCODE_DELIMITED) ? s.bytes_written : 0;
}
inline size_t wlEncodeToRow(uint8_t* out, size_t cap, const wl_ToRow& m) {
  return wlEncode(out, cap, wl_ToRow_fields, &m);
}
inline size_t wlEncodeToMaster(uint8_t* out, size_t cap, const wl_ToMaster& m) {
  return wlEncode(out, cap, wl_ToMaster_fields, &m);
}

enum class WlFeed : uint8_t { NeedMore, Message, Bad };

// Collects one direction's messages. MaxBody is the generated maximum encoded
// size of that direction's envelope (wl_ToRow_size / wl_ToMaster_size).
//
//   reader.feed(bytes, n);
//   while (reader.peek() == WlFeed::Message) {
//     if (reader.decode(wl_ToRow_fields, &msg)) handle(msg);
//     reader.pop();
//   }
//   if (reader.peek() == WlFeed::Bad) close the connection;
template <size_t MaxBody>
struct WlReader {
  uint8_t buf[WL_PREFIX_MAX + MaxBody];
  size_t have = 0;

  // Returns how many bytes were taken; fewer than n means a message is
  // waiting to be popped.
  size_t feed(const uint8_t* data, size_t n) {
    const size_t room = sizeof(buf) - have;
    const size_t take = n < room ? n : room;
    memcpy(buf + have, data, take);
    have += take;
    return take;
  }

  WlFeed peek() const {
    size_t prefix = 0, body = 0;
    return parse(prefix, body);
  }

  bool decode(const pb_msgdesc_t* fields, void* msg) const {
    size_t prefix = 0, body = 0;
    if (parse(prefix, body) != WlFeed::Message) return false;
    pb_istream_t s = pb_istream_from_buffer(buf + prefix, body);
    return pb_decode(&s, fields, msg);
  }

  void pop() {
    size_t prefix = 0, body = 0;
    if (parse(prefix, body) != WlFeed::Message) return;
    const size_t used = prefix + body;
    memmove(buf, buf + used, have - used);
    have -= used;
  }

  void reset() { have = 0; }

 private:
  WlFeed parse(size_t& prefix, size_t& body) const {
    uint32_t len = 0;
    for (size_t i = 0; i < have && i < WL_PREFIX_MAX; i++) {
      len |= (uint32_t)(buf[i] & 0x7F) << (7 * i);
      if (!(buf[i] & 0x80)) {
        if (len > MaxBody) return WlFeed::Bad;
        prefix = i + 1;
        body = len;
        return have >= prefix + body ? WlFeed::Message : WlFeed::NeedMore;
      }
    }
    return have >= WL_PREFIX_MAX ? WlFeed::Bad : WlFeed::NeedMore;
  }
};

using WlRowReader = WlReader<wl_ToRow_size>;        // on the row board
using WlMasterReader = WlReader<wl_ToMaster_size>;  // on the master

// ---- unit facts as a document in pieces --------------------------------------------
//
// The row's /units/health JSON is larger than one message, so it travels as
// consecutive UnitsJson pieces.

// Row side: fills `m` with the piece of `doc` that starts at `offset`;
// returns the offset of the next piece (== total when this was the last).
inline uint32_t wlUnitsPiece(wl_ToMaster& m, uint32_t docId, const char* doc, uint32_t total,
                             uint32_t offset) {
  m = wl_ToMaster_init_zero;
  m.which_body = wl_ToMaster_units_json_tag;
  wl_UnitsJson& u = m.body.units_json;
  u.doc_id = docId;
  u.offset = offset;
  u.total = total;
  uint32_t n = total - offset;
  if (n > sizeof(u.data.bytes)) n = sizeof(u.data.bytes);
  memcpy(u.data.bytes, doc + offset, n);
  u.data.size = (pb_size_t)n;
  return offset + n;
}

// Master side: puts the pieces back together in the caller's buffer. A piece
// out of order, from another document, or past the buffer drops the document;
// the row sends the next one in full anyway.
struct WlDocAssembler {
  enum class Result : uint8_t { Partial, Complete, Rejected };

  char* buf;
  size_t cap;  // room for the document and its terminator
  uint32_t docId = 0;
  uint32_t total = 0;
  uint32_t have = 0;
  bool active = false;

  WlDocAssembler(char* buffer, size_t capacity) : buf(buffer), cap(capacity) {}

  Result add(const wl_UnitsJson& p) {
    if (p.total == 0 || p.total >= cap || p.offset > p.total ||
        p.data.size > p.total - p.offset) {
      active = false;
      return Result::Rejected;
    }
    if (p.offset == 0) {
      docId = p.doc_id;
      total = p.total;
      have = 0;
      active = true;
    } else if (!active || p.doc_id != docId || p.total != total || p.offset != have) {
      active = false;
      return Result::Rejected;
    }
    memcpy(buf + have, p.data.bytes, p.data.size);
    have += p.data.size;
    if (have < total) return Result::Partial;
    buf[total] = 0;
    active = false;
    return Result::Complete;
  }
};
