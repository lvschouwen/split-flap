// End-to-end loopback test for the S3 <-> H2 link stack.
//
// No UART hardware involved — we simulate the wire by pumping bytes from
// one side's encoder into the other side's frame reader. Validates that the
// full stack (proto::encode* -> frame::encode -> wire -> frame::Reader ->
// proto::parse) works end-to-end.

#include <unity.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include "cobs_crc.h"
#include "frame_reader.h"
#include "protocol.h"

namespace fr = splitflap::frame;
namespace p  = splitflap::proto;

void setUp() {}
void tearDown() {}

namespace {

// Simulated endpoint with a frame reader and an outgoing byte queue.
struct Endpoint {
  static constexpr size_t kBufCap = 128;
  uint8_t scratch[kBufCap];
  uint8_t payload[kBufCap];
  fr::Reader reader{scratch, kBufCap, payload, kBufCap};
  std::vector<uint8_t> tx;  // bytes to be delivered to the peer

  // Encode a ping/pong message, then frame it, then append to tx.
  void sendPing(uint16_t seq) {
    uint8_t msg[8];
    const size_t m = p::encodePing(seq, msg, sizeof(msg));
    uint8_t w[fr::encodeBound(8)];
    const size_t n = fr::encode(msg, m, w, sizeof(w));
    tx.insert(tx.end(), w, w + n);
  }
  void sendPong(uint16_t seq) {
    uint8_t msg[8];
    const size_t m = p::encodePong(seq, msg, sizeof(msg));
    uint8_t w[fr::encodeBound(8)];
    const size_t n = fr::encode(msg, m, w, sizeof(w));
    tx.insert(tx.end(), w, w + n);
  }
};

// Pump all of src.tx into dst.reader. Invokes `on_frame` for every complete
// frame and `on_drop` for every dropped one. Clears src.tx afterward.
template <typename OnFrame, typename OnDrop>
void pump(Endpoint& src, Endpoint& dst, OnFrame on_frame, OnDrop on_drop) {
  for (uint8_t b : src.tx) {
    const auto ev = dst.reader.feed(b);
    if (ev.kind == fr::Reader::Event::Kind::Frame) {
      on_frame(dst.reader.payload(), dst.reader.payloadLen());
    } else if (ev.kind == fr::Reader::Event::Kind::Dropped) {
      on_drop(ev.err);
    }
  }
  src.tx.clear();
}

}  // namespace

// ---- happy path ----------------------------------------------------------

void test_ping_pong_loopback() {
  Endpoint s3;  // initiator
  Endpoint h2;  // responder

  // S3 sends PING.
  s3.sendPing(42);

  // H2 receives S3's bytes.
  int ping_seen = 0;
  uint16_t seen_seq = 0;
  pump(s3, h2,
       [&](const uint8_t* pl, size_t len) {
         p::Parsed parsed{};
         TEST_ASSERT_TRUE(p::parse(pl, len, &parsed));
         if (parsed.type == p::MsgType::Ping) {
           ++ping_seen;
           seen_seq = parsed.seq;
           h2.sendPong(parsed.seq);  // auto-respond
         }
       },
       [](fr::Error) { TEST_FAIL_MESSAGE("frame dropped on H2"); });

  TEST_ASSERT_EQUAL(1, ping_seen);
  TEST_ASSERT_EQUAL_UINT16(42, seen_seq);

  // S3 receives H2's PONG.
  int pong_seen = 0;
  uint16_t pong_seq = 0;
  pump(h2, s3,
       [&](const uint8_t* pl, size_t len) {
         p::Parsed parsed{};
         TEST_ASSERT_TRUE(p::parse(pl, len, &parsed));
         if (parsed.type == p::MsgType::Pong) {
           ++pong_seen;
           pong_seq = parsed.seq;
         }
       },
       [](fr::Error) { TEST_FAIL_MESSAGE("frame dropped on S3"); });

  TEST_ASSERT_EQUAL(1, pong_seen);
  TEST_ASSERT_EQUAL_UINT16(42, pong_seq);
}

// ---- burst traffic + sequence tracking ----------------------------------

void test_bursted_pings_each_match_pong() {
  Endpoint s3, h2;
  constexpr uint16_t kCount = 20;
  for (uint16_t i = 1; i <= kCount; ++i) s3.sendPing(i);

  std::vector<uint16_t> seen_pings;
  pump(s3, h2,
       [&](const uint8_t* pl, size_t len) {
         p::Parsed parsed{};
         if (p::parse(pl, len, &parsed) && parsed.type == p::MsgType::Ping) {
           seen_pings.push_back(parsed.seq);
           h2.sendPong(parsed.seq);
         }
       },
       [](fr::Error) { TEST_FAIL_MESSAGE("H2 dropped a frame"); });

  TEST_ASSERT_EQUAL_size_t(kCount, seen_pings.size());
  for (uint16_t i = 0; i < kCount; ++i) {
    TEST_ASSERT_EQUAL_UINT16(i + 1, seen_pings[i]);
  }

  std::vector<uint16_t> seen_pongs;
  pump(h2, s3,
       [&](const uint8_t* pl, size_t len) {
         p::Parsed parsed{};
         if (p::parse(pl, len, &parsed) && parsed.type == p::MsgType::Pong) {
           seen_pongs.push_back(parsed.seq);
         }
       },
       [](fr::Error) { TEST_FAIL_MESSAGE("S3 dropped a frame"); });

  TEST_ASSERT_EQUAL_size_t(kCount, seen_pongs.size());
  for (uint16_t i = 0; i < kCount; ++i) {
    TEST_ASSERT_EQUAL_UINT16(i + 1, seen_pongs[i]);
  }
}

// ---- line-noise recovery -------------------------------------------------

void test_garbage_before_frame_is_dropped_and_next_frame_survives() {
  Endpoint s3, h2;
  // Prepend random noise followed by a 0x00 to force a failed decode.
  const uint8_t noise[] = {0x11, 0x22, 0x33, 0x00};
  s3.tx.insert(s3.tx.end(), noise, noise + sizeof(noise));
  s3.sendPing(7);

  int ping_seen = 0;
  int drops = 0;
  pump(s3, h2,
       [&](const uint8_t* pl, size_t len) {
         p::Parsed parsed{};
         if (p::parse(pl, len, &parsed) && parsed.type == p::MsgType::Ping) {
           ++ping_seen;
         }
       },
       [&](fr::Error) { ++drops; });

  TEST_ASSERT_EQUAL(1, ping_seen);
  TEST_ASSERT_GREATER_OR_EQUAL(1, drops);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_ping_pong_loopback);
  RUN_TEST(test_bursted_pings_each_match_pong);
  RUN_TEST(test_garbage_before_frame_is_dropped_and_next_frame_survives);
  return UNITY_END();
}
