// Native tests for the wall link (link/wall_link.proto + WallLinkStream.h).
// Encoding and decoding are nanopb's and are not re-tested here; these tests
// pin what is ours: the schema's sizes and limits, the stream reader, and the
// unit-facts document in pieces.
//
// The VEC lines are bytes produced by this build's nanopb code. The Python
// test in link/tests decodes them with the stock protobuf library, so the
// schema is proven to mean the same on both sides.
#include <unity.h>

#include <stdio.h>

#include "WallLinkStream.h"

#define VEC(name, hex) hex

void setUp() {}
void tearDown() {}

static uint8_t wire[WL_PREFIX_MAX + wl_ToMaster_size + 8];

static void assertWire(size_t n, const char* hex) {
  TEST_ASSERT_NOT_EQUAL(0, n);
  static char got[2 * sizeof(wire) + 1];
  for (size_t i = 0; i < n; i++) snprintf(got + 2 * i, 3, "%02x", wire[i]);
  TEST_ASSERT_EQUAL_STRING(hex, got);
}

// ---- the schema on the wire -------------------------------------------------------

static void test_hello_round_trip_and_bytes() {
  wl_ToMaster m = wl_ToMaster_init_zero;
  m.which_body = wl_ToMaster_hello_tag;
  m.body.hello.protocol = WALL_LINK_PROTOCOL;
  strcpy(m.body.hello.id, "split-flap-261bb6");
  strcpy(m.body.hello.rev, "3f1a516");
  m.body.hello.boot_id = 0xDEADBEEF;
  m.body.hello.rescue = true;
  m.body.hello.width = 5;
  size_t n = wlEncodeToMaster(wire, sizeof wire, m);
  assertWire(n, VEC("hello", "2a0a280801121173706c69742d666c61702d3236316262361a073366316135313620effdb6f50d28013005"));

  WlMasterReader r;
  TEST_ASSERT_EQUAL(n, r.feed(wire, n));
  TEST_ASSERT_EQUAL(WlFeed::Message, r.peek());
  wl_ToMaster d = wl_ToMaster_init_zero;
  TEST_ASSERT_TRUE(r.decode(wl_ToMaster_fields, &d));
  TEST_ASSERT_EQUAL(wl_ToMaster_hello_tag, d.which_body);
  TEST_ASSERT_EQUAL_STRING("split-flap-261bb6", d.body.hello.id);
  TEST_ASSERT_EQUAL_STRING("3f1a516", d.body.hello.rev);
  TEST_ASSERT_EQUAL_HEX32(0xDEADBEEF, d.body.hello.boot_id);
  TEST_ASSERT_TRUE(d.body.hello.rescue);
  TEST_ASSERT_EQUAL(5, d.body.hello.width);
}

static void test_show_round_trip_and_bytes() {
  wl_ToRow m = wl_ToRow_init_zero;
  m.which_body = wl_ToRow_show_tag;
  m.body.show.render_id = 94;
  m.body.show.commit_at_ms = 1791223510400ULL;
  m.body.show.speed = 80;
  strcpy(m.body.show.text, "19:34");
  size_t n = wlEncodeToRow(wire, sizeof wire, m);
  assertWire(n, VEC("show", "141212085e1080ebf6e990341850220531393a3334"));

  WlRowReader r;
  r.feed(wire, n);
  wl_ToRow d = wl_ToRow_init_zero;
  TEST_ASSERT_TRUE(r.decode(wl_ToRow_fields, &d));
  TEST_ASSERT_EQUAL(wl_ToRow_show_tag, d.which_body);
  TEST_ASSERT_EQUAL_UINT32(94, d.body.show.render_id);
  TEST_ASSERT_TRUE(d.body.show.commit_at_ms == 1791223510400ULL);
  TEST_ASSERT_EQUAL_STRING("19:34", d.body.show.text);
}

static void test_update_and_op_round_trip_and_bytes() {
  wl_ToRow m = wl_ToRow_init_zero;
  m.which_body = wl_ToRow_update_tag;
  strcpy(m.body.update.rev, "3f1a516");
  m.body.update.size = 323047;
  for (uint8_t i = 0; i < 16; i++) m.body.update.md5[i] = i;
  m.body.update.packed = true;
  size_t n = wlEncodeToRow(wire, sizeof wire, m);
  assertWire(n, VEC("update", "2332210a073366316135313610e7db131a10000102030405060708090a0b0c0d0e0f2001"));

  wl_ToRow op = wl_ToRow_init_zero;
  op.which_body = wl_ToRow_op_tag;
  op.body.op.op_id = 0xA1B20007;
  op.body.op.opcode = 5;
  op.body.op.address = 6;
  op.body.op.args.size = 2;
  op.body.op.args.bytes[0] = 0x01;
  op.body.op.args.bytes[1] = 0xF4;
  n = wlEncodeToRow(wire, sizeof wire, op);
  assertWire(n, VEC("op", "102a0e088780c88d0a10051806220201f4"));
  WlRowReader r;
  r.feed(wire, n);
  wl_ToRow d = wl_ToRow_init_zero;
  TEST_ASSERT_TRUE(r.decode(wl_ToRow_fields, &d));
  TEST_ASSERT_EQUAL_HEX32(0xA1B20007, d.body.op.op_id);
  TEST_ASSERT_EQUAL(2, d.body.op.args.size);
  TEST_ASSERT_EQUAL_HEX8(0xF4, d.body.op.args.bytes[1]);
}

static void test_status_with_a_negative_signal_and_empty_messages() {
  wl_ToMaster m = wl_ToMaster_init_zero;
  m.which_body = wl_ToMaster_status_tag;
  m.body.status.up_s = 5429;
  m.body.status.heap = 27312;
  m.body.status.min_heap = 17464;
  m.body.status.rssi = -58;
  m.body.status.busy = true;
  size_t n = wlEncodeToMaster(wire, sizeof wire, m);
  assertWire(n, VEC("status", "11120f08b52a10b0d50118b8880128736001"));
  WlMasterReader r;
  r.feed(wire, n);
  wl_ToMaster d = wl_ToMaster_init_zero;
  TEST_ASSERT_TRUE(r.decode(wl_ToMaster_fields, &d));
  TEST_ASSERT_EQUAL(-58, d.body.status.rssi);
  TEST_ASSERT_TRUE(d.body.status.busy);

  wl_ToRow ping = wl_ToRow_init_zero;
  ping.which_body = wl_ToRow_ping_tag;
  assertWire(wlEncodeToRow(wire, sizeof wire, ping), VEC("ping", "024200"));
  wl_ToMaster pong = wl_ToMaster_init_zero;
  pong.which_body = wl_ToMaster_pong_tag;
  assertWire(wlEncodeToMaster(wire, sizeof wire, pong), VEC("pong", "024200"));
}

// ---- sizes and limits ---------------------------------------------------------------

static void test_the_row_only_ever_buffers_a_small_message() {
  // What the row board must hold for one incoming message. Text for a row,
  // a tz rule, an update notice: nothing large travels master -> row.
  TEST_ASSERT_TRUE(wl_ToRow_size <= 96);
  TEST_ASSERT_TRUE(wl_ToMaster_size <= 512);
  TEST_ASSERT_TRUE(sizeof(WlRowReader) <= 112);
}

static void test_every_message_at_its_largest_fits_its_reader() {
  wl_ToMaster m = wl_ToMaster_init_zero;
  m.which_body = wl_ToMaster_op_state_tag;
  m.body.op_state.op_id = 0xFFFFFFFF;
  m.body.op_state.phase = wl_OpPhase_OP_REFUSED;
  m.body.op_state.reason = 0xFFFFFFFF;
  m.body.op_state.data_offset = 0xFFFFFFFF;
  m.body.op_state.data.size = sizeof(m.body.op_state.data.bytes);
  memset(m.body.op_state.data.bytes, 0xFF, sizeof(m.body.op_state.data.bytes));
  size_t n = wlEncodeToMaster(wire, sizeof wire, m);
  TEST_ASSERT_NOT_EQUAL(0, n);
  WlMasterReader r;
  TEST_ASSERT_EQUAL(n, r.feed(wire, n));
  TEST_ASSERT_EQUAL(WlFeed::Message, r.peek());
  static wl_ToMaster d;
  TEST_ASSERT_TRUE(r.decode(wl_ToMaster_fields, &d));
  TEST_ASSERT_EQUAL(480, d.body.op_state.data.size);
}

static void test_a_text_longer_than_a_row_is_refused_on_decode() {
  // ToRow{show{text: 17 x 'A'}}, written by a peer that does not enforce the limit.
  uint8_t over[32] = {0x15, 0x12, 0x13, 0x22, 0x11};
  memset(over + 5, 'A', 17);
  WlRowReader r;
  r.feed(over, 5 + 17);
  TEST_ASSERT_EQUAL(WlFeed::Message, r.peek());
  wl_ToRow d = wl_ToRow_init_zero;
  TEST_ASSERT_FALSE(r.decode(wl_ToRow_fields, &d));
}

static void test_a_message_that_does_not_fit_is_not_encoded() {
  wl_ToMaster m = wl_ToMaster_init_zero;
  m.which_body = wl_ToMaster_hello_tag;
  strcpy(m.body.hello.id, "split-flap-261bb6");
  uint8_t small[8];
  TEST_ASSERT_EQUAL(0, wlEncodeToMaster(small, sizeof small, m));
}

static void test_fields_and_messages_from_a_newer_build_are_ignored() {
  // ToRow{quiet{on: true, <field 15>: 7}} — a field this build does not know.
  const uint8_t newerField[] = {0x06, 0x1A, 0x04, 0x08, 0x01, 0x78, 0x07};
  WlRowReader r;
  r.feed(newerField, sizeof newerField);
  wl_ToRow d = wl_ToRow_init_zero;
  TEST_ASSERT_TRUE(r.decode(wl_ToRow_fields, &d));
  TEST_ASSERT_EQUAL(wl_ToRow_quiet_tag, d.which_body);
  TEST_ASSERT_TRUE(d.body.quiet.on);
  r.pop();

  // ToRow{<member 30>: {}} — a message this build does not know: nothing to act on.
  const uint8_t newerMessage[] = {0x03, 0xF2, 0x01, 0x00};
  r.feed(newerMessage, sizeof newerMessage);
  d = wl_ToRow_init_zero;
  TEST_ASSERT_TRUE(r.decode(wl_ToRow_fields, &d));
  TEST_ASSERT_EQUAL(0, d.which_body);
}

// ---- stream reader ---------------------------------------------------------------------

static size_t twoMessages(uint8_t* out, size_t cap) {
  wl_ToRow a = wl_ToRow_init_zero;
  a.which_body = wl_ToRow_quiet_tag;
  a.body.quiet.on = true;
  size_t n = wlEncodeToRow(out, cap, a);
  wl_ToRow b = wl_ToRow_init_zero;
  b.which_body = wl_ToRow_show_tag;
  b.body.show.render_id = 9;
  strcpy(b.body.show.text, "HELLO");
  return n + wlEncodeToRow(out + n, cap - n, b);
}

static void test_reader_reassembles_messages_fed_one_byte_at_a_time() {
  uint8_t stream[64];
  const size_t n = twoMessages(stream, sizeof stream);
  WlRowReader r;
  int seen = 0;
  pb_size_t which[2] = {0, 0};
  for (size_t i = 0; i < n; i++) {
    TEST_ASSERT_EQUAL(1, r.feed(stream + i, 1));
    while (r.peek() == WlFeed::Message) {
      wl_ToRow d = wl_ToRow_init_zero;
      TEST_ASSERT_TRUE(r.decode(wl_ToRow_fields, &d));
      which[seen++] = d.which_body;
      r.pop();
    }
  }
  TEST_ASSERT_EQUAL(2, seen);
  TEST_ASSERT_EQUAL(wl_ToRow_quiet_tag, which[0]);
  TEST_ASSERT_EQUAL(wl_ToRow_show_tag, which[1]);
  TEST_ASSERT_EQUAL(0, r.have);
}

static void test_reader_takes_two_messages_from_one_read() {
  uint8_t stream[64];
  const size_t n = twoMessages(stream, sizeof stream);
  WlRowReader r;
  TEST_ASSERT_EQUAL(n, r.feed(stream, n));
  TEST_ASSERT_EQUAL(WlFeed::Message, r.peek());
  r.pop();
  wl_ToRow d = wl_ToRow_init_zero;
  TEST_ASSERT_TRUE(r.decode(wl_ToRow_fields, &d));
  TEST_ASSERT_EQUAL_STRING("HELLO", d.body.show.text);
  r.pop();
  TEST_ASSERT_EQUAL(WlFeed::NeedMore, r.peek());
}

static void test_reader_rejects_a_length_over_the_maximum() {
  WlRowReader r;
  const uint8_t tooLong[] = {(uint8_t)(wl_ToRow_size + 1)};
  r.feed(tooLong, 1);
  TEST_ASSERT_EQUAL(WlFeed::Bad, r.peek());

  WlRowReader endless;
  const uint8_t noEnd[] = {0x80, 0x80, 0x80};  // a length that never finishes
  endless.feed(noEnd, sizeof noEnd);
  TEST_ASSERT_EQUAL(WlFeed::Bad, endless.peek());
}

static void test_reader_stops_taking_bytes_when_full_and_resumes_after_pop() {
  static uint8_t stream[2 * (WL_PREFIX_MAX + wl_ToMaster_size)];
  wl_ToMaster m = wl_ToMaster_init_zero;
  m.which_body = wl_ToMaster_log_line_tag;
  m.body.log_line.text.size = sizeof(m.body.log_line.text.bytes);
  memset(m.body.log_line.text.bytes, 'x', sizeof(m.body.log_line.text.bytes));
  size_t one = wlEncodeToMaster(stream, sizeof stream, m);
  size_t n = one + wlEncodeToMaster(stream + one, sizeof stream - one, m);
  static WlMasterReader r;
  r.reset();
  size_t fed = 0;
  int seen = 0;
  for (int guard = 0; guard < 8 && fed < n; guard++) {
    fed += r.feed(stream + fed, n - fed);
    while (r.peek() == WlFeed::Message) {
      seen++;
      r.pop();
    }
  }
  TEST_ASSERT_EQUAL(n, fed);
  TEST_ASSERT_EQUAL(2, seen);
}

static void test_a_message_that_does_not_parse_can_be_skipped() {
  // Right length, body is not a valid message (a field cut short).
  const uint8_t garbage[] = {0x02, 0x12, 0x7F};
  uint8_t stream[32];
  memcpy(stream, garbage, sizeof garbage);
  wl_ToRow ok = wl_ToRow_init_zero;
  ok.which_body = wl_ToRow_ping_tag;
  size_t n = sizeof garbage + wlEncodeToRow(stream + sizeof garbage, sizeof stream - sizeof garbage, ok);
  WlRowReader r;
  r.feed(stream, n);
  wl_ToRow d = wl_ToRow_init_zero;
  TEST_ASSERT_EQUAL(WlFeed::Message, r.peek());
  TEST_ASSERT_FALSE(r.decode(wl_ToRow_fields, &d));
  r.pop();
  TEST_ASSERT_TRUE(r.decode(wl_ToRow_fields, &d));
  TEST_ASSERT_EQUAL(wl_ToRow_ping_tag, d.which_body);
}

// ---- unit facts in pieces -----------------------------------------------------------------

static char doc[1300];
static char rebuilt[1400];

static void makeDoc() {
  for (size_t i = 0; i < sizeof doc; i++) doc[i] = (char)('a' + i % 26);
}

static void test_a_document_travels_in_pieces_and_comes_back_whole() {
  makeDoc();
  WlDocAssembler a(rebuilt, sizeof rebuilt);
  static wl_ToMaster m;
  static wl_ToMaster d;
  uint32_t offset = 0;
  int pieces = 0;
  WlDocAssembler::Result res = WlDocAssembler::Result::Partial;
  while (offset < sizeof doc) {
    offset = wlUnitsPiece(m, 7, doc, sizeof doc, offset);
    size_t n = wlEncodeToMaster(wire, sizeof wire, m);
    TEST_ASSERT_NOT_EQUAL(0, n);
    static WlMasterReader r;
    r.reset();
    r.feed(wire, n);
    TEST_ASSERT_TRUE(r.decode(wl_ToMaster_fields, &d));
    res = a.add(d.body.units_json);
    pieces++;
    TEST_ASSERT_TRUE(res != WlDocAssembler::Result::Rejected);
  }
  TEST_ASSERT_EQUAL(3, pieces);  // 480 + 480 + 340
  TEST_ASSERT_TRUE(res == WlDocAssembler::Result::Complete);
  TEST_ASSERT_EQUAL(sizeof doc, strlen(rebuilt));
  TEST_ASSERT_EQUAL_MEMORY(doc, rebuilt, sizeof doc);
}

static void test_a_piece_out_of_order_or_from_another_document_drops_it() {
  makeDoc();
  static wl_ToMaster first, second, other;
  wlUnitsPiece(first, 7, doc, sizeof doc, 0);
  wlUnitsPiece(second, 7, doc, sizeof doc, 480);
  wlUnitsPiece(other, 8, doc, sizeof doc, 480);

  WlDocAssembler a(rebuilt, sizeof rebuilt);
  TEST_ASSERT_TRUE(a.add(second.body.units_json) == WlDocAssembler::Result::Rejected);  // no start
  TEST_ASSERT_TRUE(a.add(first.body.units_json) == WlDocAssembler::Result::Partial);
  TEST_ASSERT_TRUE(a.add(other.body.units_json) == WlDocAssembler::Result::Rejected);   // other document
  TEST_ASSERT_TRUE(a.add(second.body.units_json) == WlDocAssembler::Result::Rejected);  // dropped: needs a new start
  TEST_ASSERT_TRUE(a.add(first.body.units_json) == WlDocAssembler::Result::Partial);
  TEST_ASSERT_TRUE(a.add(first.body.units_json) == WlDocAssembler::Result::Partial);    // a restart is a new start
  TEST_ASSERT_TRUE(a.add(second.body.units_json) == WlDocAssembler::Result::Partial);
}

static void test_a_document_larger_than_the_buffer_or_lying_about_its_size_is_refused() {
  makeDoc();
  static wl_ToMaster m;
  wlUnitsPiece(m, 1, doc, sizeof doc, 0);
  char tiny[64];
  WlDocAssembler small(tiny, sizeof tiny);
  TEST_ASSERT_TRUE(small.add(m.body.units_json) == WlDocAssembler::Result::Rejected);

  WlDocAssembler exact(rebuilt, sizeof doc);  // no room for the terminator
  TEST_ASSERT_TRUE(exact.add(m.body.units_json) == WlDocAssembler::Result::Rejected);

  WlDocAssembler a(rebuilt, sizeof rebuilt);
  m.body.units_json.total = 100;  // 480 bytes of data in a 100-byte document
  TEST_ASSERT_TRUE(a.add(m.body.units_json) == WlDocAssembler::Result::Rejected);
  m.body.units_json.total = 0;
  TEST_ASSERT_TRUE(a.add(m.body.units_json) == WlDocAssembler::Result::Rejected);
}

static void test_a_short_document_is_complete_in_one_piece() {
  const char* small = "{\"width\":0,\"faulty\":0,\"units\":[]}";
  static wl_ToMaster m;
  TEST_ASSERT_EQUAL(strlen(small), wlUnitsPiece(m, 3, small, strlen(small), 0));
  WlDocAssembler a(rebuilt, sizeof rebuilt);
  TEST_ASSERT_TRUE(a.add(m.body.units_json) == WlDocAssembler::Result::Complete);
  TEST_ASSERT_EQUAL_STRING(small, rebuilt);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_hello_round_trip_and_bytes);
  RUN_TEST(test_show_round_trip_and_bytes);
  RUN_TEST(test_update_and_op_round_trip_and_bytes);
  RUN_TEST(test_status_with_a_negative_signal_and_empty_messages);
  RUN_TEST(test_the_row_only_ever_buffers_a_small_message);
  RUN_TEST(test_every_message_at_its_largest_fits_its_reader);
  RUN_TEST(test_a_text_longer_than_a_row_is_refused_on_decode);
  RUN_TEST(test_a_message_that_does_not_fit_is_not_encoded);
  RUN_TEST(test_fields_and_messages_from_a_newer_build_are_ignored);
  RUN_TEST(test_reader_reassembles_messages_fed_one_byte_at_a_time);
  RUN_TEST(test_reader_takes_two_messages_from_one_read);
  RUN_TEST(test_reader_rejects_a_length_over_the_maximum);
  RUN_TEST(test_reader_stops_taking_bytes_when_full_and_resumes_after_pop);
  RUN_TEST(test_a_message_that_does_not_parse_can_be_skipped);
  RUN_TEST(test_a_document_travels_in_pieces_and_comes_back_whole);
  RUN_TEST(test_a_piece_out_of_order_or_from_another_document_drops_it);
  RUN_TEST(test_a_document_larger_than_the_buffer_or_lying_about_its_size_is_refused);
  RUN_TEST(test_a_short_document_is_complete_in_one_piece);
  return UNITY_END();
}
