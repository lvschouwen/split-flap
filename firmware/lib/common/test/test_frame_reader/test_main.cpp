// Host-side tests for splitflap::frame::Reader.

#include <unity.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include "cobs_crc.h"
#include "frame_reader.h"

namespace fr = splitflap::frame;

void setUp() {}
void tearDown() {}

namespace {

// Helper: encode a payload to a wire buffer (incl. trailing 0x00).
std::vector<uint8_t> encodeFrame(const std::vector<uint8_t>& payload) {
  std::vector<uint8_t> wire(fr::encodeBound(payload.size()));
  const size_t n = fr::encode(payload.data(), payload.size(),
                              wire.data(), wire.size());
  wire.resize(n);
  return wire;
}

}  // namespace

// ---- basic flow -----------------------------------------------------------

void test_reader_no_event_while_accumulating() {
  uint8_t scratch[64], payload[64];
  fr::Reader r(scratch, sizeof(scratch), payload, sizeof(payload));
  const auto wire = encodeFrame({0xAA, 0xBB, 0xCC});
  // All bytes except the last (which is the 0x00 delimiter) yield None.
  for (size_t i = 0; i + 1 < wire.size(); ++i) {
    const auto ev = r.feed(wire[i]);
    TEST_ASSERT_EQUAL(static_cast<int>(fr::Reader::Event::Kind::None),
                      static_cast<int>(ev.kind));
  }
}

void test_reader_emits_frame_on_delimiter() {
  uint8_t scratch[64], payload[64];
  fr::Reader r(scratch, sizeof(scratch), payload, sizeof(payload));
  const std::vector<uint8_t> p = {0xAA, 0xBB, 0xCC};
  const auto wire = encodeFrame(p);

  fr::Reader::Event last{};
  for (uint8_t b : wire) last = r.feed(b);

  TEST_ASSERT_EQUAL(static_cast<int>(fr::Reader::Event::Kind::Frame),
                    static_cast<int>(last.kind));
  TEST_ASSERT_EQUAL_size_t(p.size(), r.payloadLen());
  TEST_ASSERT_EQUAL_MEMORY(p.data(), r.payload(), p.size());
}

void test_reader_handles_multiple_frames_back_to_back() {
  uint8_t scratch[64], payload[64];
  fr::Reader r(scratch, sizeof(scratch), payload, sizeof(payload));

  const std::vector<uint8_t> p1 = {'a', 'b'};
  const std::vector<uint8_t> p2 = {'c', 'd', 'e'};
  auto w1 = encodeFrame(p1);
  auto w2 = encodeFrame(p2);
  std::vector<uint8_t> stream;
  stream.insert(stream.end(), w1.begin(), w1.end());
  stream.insert(stream.end(), w2.begin(), w2.end());

  int frames = 0;
  std::vector<uint8_t> got_last;
  for (uint8_t b : stream) {
    const auto ev = r.feed(b);
    if (ev.kind == fr::Reader::Event::Kind::Frame) {
      ++frames;
      got_last.assign(r.payload(), r.payload() + r.payloadLen());
    }
  }
  TEST_ASSERT_EQUAL(2, frames);
  TEST_ASSERT_EQUAL_size_t(p2.size(), got_last.size());
  TEST_ASSERT_EQUAL_MEMORY(p2.data(), got_last.data(), p2.size());
}

void test_reader_ignores_consecutive_delimiters() {
  uint8_t scratch[64], payload[64];
  fr::Reader r(scratch, sizeof(scratch), payload, sizeof(payload));

  // Three stray 0x00 bytes before the real frame — reader should ignore them.
  for (int i = 0; i < 3; ++i) {
    const auto ev = r.feed(0x00);
    TEST_ASSERT_EQUAL(static_cast<int>(fr::Reader::Event::Kind::None),
                      static_cast<int>(ev.kind));
  }
  const std::vector<uint8_t> p = {'x'};
  const auto wire = encodeFrame(p);
  fr::Reader::Event last{};
  for (uint8_t b : wire) last = r.feed(b);
  TEST_ASSERT_EQUAL(static_cast<int>(fr::Reader::Event::Kind::Frame),
                    static_cast<int>(last.kind));
}

// ---- error paths ----------------------------------------------------------

void test_reader_reports_bad_crc_on_flipped_bit() {
  uint8_t scratch[64], payload[64];
  fr::Reader r(scratch, sizeof(scratch), payload, sizeof(payload));

  auto wire = encodeFrame({'p', 'q', 'r', 's'});
  wire[wire.size() / 2] ^= 0x01;  // flip a bit in the middle

  fr::Reader::Event last{};
  for (uint8_t b : wire) last = r.feed(b);
  TEST_ASSERT_EQUAL(static_cast<int>(fr::Reader::Event::Kind::Dropped),
                    static_cast<int>(last.kind));
  TEST_ASSERT_TRUE(last.err == fr::Error::BadCrc ||
                   last.err == fr::Error::InvalidCobs);
}

void test_reader_reports_overflow_on_oversized_frame() {
  // Scratch too small to hold the encoded frame — expect BufferTooSmall
  // when the delimiter finally arrives.
  uint8_t scratch[4], payload[64];
  fr::Reader r(scratch, sizeof(scratch), payload, sizeof(payload));

  const std::vector<uint8_t> p(40, 0xAB);  // encoded will be >4 bytes
  const auto wire = encodeFrame(p);

  fr::Reader::Event last{};
  for (uint8_t b : wire) last = r.feed(b);
  TEST_ASSERT_EQUAL(static_cast<int>(fr::Reader::Event::Kind::Dropped),
                    static_cast<int>(last.kind));
  TEST_ASSERT_EQUAL(static_cast<int>(fr::Error::BufferTooSmall),
                    static_cast<int>(last.err));
}

void test_reader_recovers_after_overflow() {
  // After an overflow, feeding a valid frame should work normally.
  uint8_t scratch[8], payload[64];
  fr::Reader r(scratch, sizeof(scratch), payload, sizeof(payload));

  // Drop a big one first.
  const auto big = encodeFrame(std::vector<uint8_t>(40, 0xAB));
  for (uint8_t b : big) (void)r.feed(b);

  // Then a small one should succeed.
  const std::vector<uint8_t> p = {'y'};
  const auto wire = encodeFrame(p);
  fr::Reader::Event last{};
  for (uint8_t b : wire) last = r.feed(b);
  TEST_ASSERT_EQUAL(static_cast<int>(fr::Reader::Event::Kind::Frame),
                    static_cast<int>(last.kind));
  TEST_ASSERT_EQUAL_UINT8(p[0], r.payload()[0]);
}

void test_reader_reset_clears_in_progress_state() {
  uint8_t scratch[64], payload[64];
  fr::Reader r(scratch, sizeof(scratch), payload, sizeof(payload));

  // Feed partial garbage.
  r.feed(0xAA);
  r.feed(0xBB);
  r.reset();

  const std::vector<uint8_t> p = {'z'};
  const auto wire = encodeFrame(p);
  fr::Reader::Event last{};
  for (uint8_t b : wire) last = r.feed(b);
  TEST_ASSERT_EQUAL(static_cast<int>(fr::Reader::Event::Kind::Frame),
                    static_cast<int>(last.kind));
  TEST_ASSERT_EQUAL_UINT8(p[0], r.payload()[0]);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_reader_no_event_while_accumulating);
  RUN_TEST(test_reader_emits_frame_on_delimiter);
  RUN_TEST(test_reader_handles_multiple_frames_back_to_back);
  RUN_TEST(test_reader_ignores_consecutive_delimiters);
  RUN_TEST(test_reader_reports_bad_crc_on_flipped_bit);
  RUN_TEST(test_reader_reports_overflow_on_oversized_frame);
  RUN_TEST(test_reader_recovers_after_overflow);
  RUN_TEST(test_reader_reset_clears_in_progress_state);
  return UNITY_END();
}
