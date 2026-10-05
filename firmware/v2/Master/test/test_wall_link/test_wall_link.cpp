// Native tests for link/WallLink.h: the frame format and every message, the
// stream decoder, the link clock and the master's pacing rule.
//
// The VEC lines are the byte contract with the Python twin: link/tests reads
// them out of this file and requires link/wall_link.py to produce the same.
#include <unity.h>

#include <stdio.h>

#include "WallLink.h"
#include "round_trips_560.h"

#define VEC(name, hex) hex

void setUp() {}
void tearDown() {}

static uint8_t frame[WL_MAX_FRAME];

static void assertFrame(size_t n, const char* hex) {
  TEST_ASSERT_NOT_EQUAL(0, n);
  char got[2 * WL_MAX_FRAME + 1];
  for (size_t i = 0; i < n; i++) snprintf(got + 2 * i, 3, "%02x", frame[i]);
  TEST_ASSERT_EQUAL_STRING(hex, got);
}
static const uint8_t* payload() { return frame + WL_HEADER_LEN; }
static size_t payloadLen(size_t n) { return n - WL_HEADER_LEN; }

// ---- messages: bytes, then a decode of those bytes ----------------------------

static void test_hello() {
  WlHello m;
  strcpy(m.id, "split-flap-261bb6");
  strcpy(m.rev, "3f1a516");
  m.bootId = 0xDEADBEEF;
  m.flags = WL_HELLO_RESCUE;
  m.width = 5;
  size_t n = wlEncodeHello(frame, sizeof frame, m);
  assertFrame(n, VEC("hello", "002120011173706c69742d666c61702d3236316262360733663161353136deadbeef0105"));
  WlHello d;
  TEST_ASSERT_TRUE(wlDecodeHello(payload(), payloadLen(n), d));
  TEST_ASSERT_EQUAL(WALL_LINK_PROTOCOL, d.protocol);
  TEST_ASSERT_EQUAL_STRING("split-flap-261bb6", d.id);
  TEST_ASSERT_EQUAL_STRING("3f1a516", d.rev);
  TEST_ASSERT_EQUAL_HEX32(0xDEADBEEF, d.bootId);
  TEST_ASSERT_EQUAL(1, d.flags);
  TEST_ASSERT_EQUAL(5, d.width);
}

static void test_welcome() {
  WlWelcome m;
  strcpy(m.masterId, "split-flap-c8a746");
  size_t n = wlEncodeWelcome(frame, sizeof frame, m);
  assertFrame(n, VEC("welcome", "001301011173706c69742d666c61702d633861373436"));
  WlWelcome d;
  TEST_ASSERT_TRUE(wlDecodeWelcome(payload(), payloadLen(n), d));
  TEST_ASSERT_EQUAL_STRING("split-flap-c8a746", d.masterId);
}

static void test_time_request_and_reply() {
  WlTimeReq q;
  q.rowMs = 39153;
  q.rxCount = 17;
  size_t n = wlEncodeTimeReq(frame, sizeof frame, q);
  assertFrame(n, VEC("timereq", "000621000098f10011"));
  WlTimeReq dq;
  TEST_ASSERT_TRUE(wlDecodeTimeReq(payload(), payloadLen(n), dq));
  TEST_ASSERT_EQUAL_UINT32(39153, dq.rowMs);
  TEST_ASSERT_EQUAL(17, dq.rxCount);

  WlTime t;
  t.echoRowMs = 39153;
  t.masterMs = 7200123;
  t.epochS = 1791223510;
  n = wlEncodeTime(frame, sizeof frame, t);
  assertFrame(n, VEC("time", "000c02000098f1006ddd7b6ac3e6d6"));
  WlTime dt;
  TEST_ASSERT_TRUE(wlDecodeTime(payload(), payloadLen(n), dt));
  TEST_ASSERT_EQUAL_UINT32(7200123, dt.masterMs);
  TEST_ASSERT_EQUAL_UINT32(1791223510, dt.epochS);
}

static void test_show_and_shown() {
  WlShow m;
  m.renderId = 94;
  m.atMs = 7202000;
  m.speed = 80;
  strcpy(m.text, "19:34");
  size_t n = wlEncodeShow(frame, sizeof frame, m);
  assertFrame(n, VEC("show", "000f030000005e006de4d0500531393a3334"));
  WlShow d;
  TEST_ASSERT_TRUE(wlDecodeShow(payload(), payloadLen(n), d));
  TEST_ASSERT_EQUAL_UINT32(94, d.renderId);
  TEST_ASSERT_EQUAL_UINT32(7202000, d.atMs);
  TEST_ASSERT_EQUAL(80, d.speed);
  TEST_ASSERT_EQUAL_STRING("19:34", d.text);

  WlShow blank;
  blank.renderId = 95;
  blank.speed = 1;
  n = wlEncodeShow(frame, sizeof frame, blank);
  assertFrame(n, VEC("show_blank", "000a030000005f000000000100"));
  TEST_ASSERT_TRUE(wlDecodeShow(payload(), payloadLen(n), d));
  TEST_ASSERT_EQUAL_STRING("", d.text);

  WlShown s;
  s.renderId = 94;
  s.rxCount = 18;
  n = wlEncodeShown(frame, sizeof frame, s);
  assertFrame(n, VEC("shown", "0008230000005e00000012"));
  WlShown ds;
  TEST_ASSERT_TRUE(wlDecodeShown(payload(), payloadLen(n), ds));
  TEST_ASSERT_EQUAL(18, ds.rxCount);
}

static void test_quiet_config_logctl() {
  WlQuiet q;
  q.on = 1;
  assertFrame(wlEncodeQuiet(frame, sizeof frame, q), VEC("quiet", "00010401"));

  WlConfig c;
  c.fallback = (uint8_t)WlFallback::Time;
  c.updateUnitsAtStart = 0;
  strcpy(c.tz, "CET-1CEST,M3.5.0,M10.5.0/3");
  size_t n = wlEncodeConfig(frame, sizeof frame, c);
  assertFrame(n, VEC("config", "001d0501001a4345542d31434553542c4d332e352e302c4d31302e352e302f33"));
  WlConfig d;
  TEST_ASSERT_TRUE(wlDecodeConfig(payload(), payloadLen(n), d));
  TEST_ASSERT_EQUAL(1, d.fallback);
  TEST_ASSERT_EQUAL(0, d.updateUnitsAtStart);
  TEST_ASSERT_EQUAL_STRING("CET-1CEST,M3.5.0,M10.5.0/3", d.tz);

  WlLogCtl l;
  l.on = 1;
  assertFrame(wlEncodeLogCtl(frame, sizeof frame, l), VEC("logctl", "00010801"));
}

static void test_op_and_its_state() {
  WlOp m;
  m.opId = 0xA1B20007;
  m.opcode = 5;
  m.address = 6;
  m.argsLen = 2;
  m.args[0] = 0x01;
  m.args[1] = 0xF4;
  size_t n = wlEncodeOp(frame, sizeof frame, m);
  assertFrame(n, VEC("op", "000906a1b2000705060201f4"));
  WlOp d;
  TEST_ASSERT_TRUE(wlDecodeOp(payload(), payloadLen(n), d));
  TEST_ASSERT_EQUAL_HEX32(0xA1B20007, d.opId);
  TEST_ASSERT_EQUAL(6, d.address);
  TEST_ASSERT_EQUAL(2, d.argsLen);
  TEST_ASSERT_EQUAL_HEX8(0xF4, d.args[1]);

  const uint8_t data[] = {0x0C, 0xA2, 0x0C};
  WlOpState s;
  s.opId = 0xA1B20007;
  s.phase = (uint8_t)WlOpPhase::Ok;
  s.rxCount = 19;
  s.dataOffset = 128;
  s.dataLen = sizeof data;
  s.data = data;
  n = wlEncodeOpState(frame, sizeof frame, s);
  assertFrame(n, VEC("opstate", "000f24a1b2000701000013008000030ca20c"));
  WlOpState ds;
  TEST_ASSERT_TRUE(wlDecodeOpState(payload(), payloadLen(n), ds));
  TEST_ASSERT_EQUAL(128, ds.dataOffset);
  TEST_ASSERT_EQUAL(3, ds.dataLen);
  TEST_ASSERT_EQUAL_HEX8_ARRAY(data, ds.data, 3);
}

static void test_update() {
  WlUpdate m;
  strcpy(m.rev, "3f1a516");
  m.size = 323047;
  for (uint8_t i = 0; i < 16; i++) m.md5[i] = i;
  m.packed = 1;
  size_t n = wlEncodeUpdate(frame, sizeof frame, m);
  assertFrame(n, VEC("update", "001d0707336631613531360004ede7000102030405060708090a0b0c0d0e0f01"));
  WlUpdate d;
  TEST_ASSERT_TRUE(wlDecodeUpdate(payload(), payloadLen(n), d));
  TEST_ASSERT_EQUAL_STRING("3f1a516", d.rev);
  TEST_ASSERT_EQUAL_UINT32(323047, d.size);
  TEST_ASSERT_EQUAL_HEX8_ARRAY(m.md5, d.md5, 16);
  TEST_ASSERT_EQUAL(1, d.packed);
}

static void test_status_event_logline_pong() {
  WlStatus m;
  m.rxCount = 17;
  m.upS = 5429;
  m.heap = 27312;
  m.minHeap = 17464;
  m.maxBlock = 26232;
  m.rssi = -58;
  m.txPower = 20;
  m.busTx = 13502;
  m.busEpisodes = 2;
  m.jobRunning = 1;
  m.imageSize = 461600;
  size_t n = wlEncodeStatus(frame, sizeof frame, m);
  assertFrame(n, VEC("status", "00252200110000153500006ab00000443800006678c614000034be00000000000002000100070b20"));
  WlStatus d;
  TEST_ASSERT_TRUE(wlDecodeStatus(payload(), payloadLen(n), d));
  TEST_ASSERT_EQUAL(-58, d.rssi);
  TEST_ASSERT_EQUAL_UINT32(17464, d.minHeap);
  TEST_ASSERT_EQUAL(2, d.busEpisodes);
  TEST_ASSERT_EQUAL(1, d.jobRunning);
  TEST_ASSERT_EQUAL_UINT32(461600, d.imageSize);

  WlEvent e;
  e.code = 3;
  e.unit = 6;
  e.a = 10;
  e.upS = 8343;
  n = wlEncodeEvent(frame, sizeof frame, e);
  assertFrame(n, VEC("event", "000f250003060000000a0000000000002097"));
  WlEvent de;
  TEST_ASSERT_TRUE(wlDecodeEvent(payload(), payloadLen(n), de));
  TEST_ASSERT_EQUAL(6, de.unit);
  TEST_ASSERT_EQUAL_UINT32(8343, de.upS);

  const char* line = "[5429] bus recovered";
  assertFrame(wlEncodeLogLine(frame, sizeof frame, line, strlen(line)),
              VEC("logline", "0014265b353432395d20627573207265636f7665726564"));

  WlPong p;
  p.rxCount = 65535;
  n = wlEncodePong(frame, sizeof frame, p);
  assertFrame(n, VEC("pong", "000227ffff"));
  WlPong dp;
  TEST_ASSERT_TRUE(wlDecodePong(payload(), payloadLen(n), dp));
  TEST_ASSERT_EQUAL(65535, dp.rxCount);
}

static void test_empty_messages() {
  assertFrame(wlEncodeEmpty(frame, sizeof frame, WlType::Ping), VEC("ping", "000009"));
  assertFrame(wlEncodeEmpty(frame, sizeof frame, WlType::Restart), VEC("restart", "00000a"));
  assertFrame(wlEncodeEmpty(frame, sizeof frame, WlType::Release), VEC("release", "00000b"));
}

// ---- rules of the format --------------------------------------------------------

static void test_a_field_over_its_limit_fails_the_frame() {
  WlShow m;
  memset(m.text, 'A', WL_TEXT_MAX);
  m.text[WL_TEXT_MAX] = 0;
  TEST_ASSERT_NOT_EQUAL(0, wlEncodeShow(frame, sizeof frame, m));

  // One character over the row limit, sent by a peer that does not enforce it.
  uint8_t over[32] = {0, 0, 0, 94, 0, 0, 0, 0, 80, (uint8_t)(WL_TEXT_MAX + 1)};
  WlShow d;
  TEST_ASSERT_FALSE(wlDecodeShow(over, 10 + WL_TEXT_MAX + 1, d));
  TEST_ASSERT_EQUAL_STRING("", d.text);

  WlOp op;
  op.argsLen = WL_OP_ARGS_MAX + 1;
  TEST_ASSERT_EQUAL(0, wlEncodeOp(frame, sizeof frame, op));
}

static void test_a_frame_that_does_not_fit_is_not_built() {
  WlHello m;
  strcpy(m.id, "split-flap-261bb6");
  uint8_t small[16];
  TEST_ASSERT_EQUAL(0, wlEncodeHello(small, sizeof small, m));
  TEST_ASSERT_EQUAL(0, wlEncodeEmpty(small, 2, WlType::Ping));
}

static void test_a_cut_message_fails_to_decode() {
  WlStatus m;
  size_t n = wlEncodeStatus(frame, sizeof frame, m);
  WlStatus d;
  TEST_ASSERT_FALSE(wlDecodeStatus(payload(), payloadLen(n) - 1, d));
  WlPong p;
  TEST_ASSERT_FALSE(wlDecodePong(payload(), 1, p));
}

static void test_a_message_may_grow_at_its_end() {
  WlPong m;
  m.rxCount = 7;
  size_t n = wlEncodePong(frame, sizeof frame, m);
  frame[n] = 0xAB;      // a field a later build added
  frame[n + 1] = 0xCD;
  WlPong d;
  TEST_ASSERT_TRUE(wlDecodePong(payload(), payloadLen(n) + 2, d));
  TEST_ASSERT_EQUAL(7, d.rxCount);
}

static void test_op_state_data_cannot_reach_past_the_payload() {
  // dataLen says 3, one byte follows.
  const uint8_t p[] = {0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 3, 0xAA};
  WlOpState d;
  TEST_ASSERT_FALSE(wlDecodeOpState(p, sizeof p, d));
}

static void test_master_and_row_types_do_not_overlap() {
  TEST_ASSERT_TRUE(wlTypeFromMaster((uint8_t)WlType::Show));
  TEST_ASSERT_TRUE(wlTypeFromMaster((uint8_t)WlType::Release));
  TEST_ASSERT_FALSE(wlTypeFromMaster((uint8_t)WlType::Hello));
  TEST_ASSERT_FALSE(wlTypeFromMaster((uint8_t)WlType::Pong));
}

// ---- stream decoder ---------------------------------------------------------------

static void test_decoder_reassembles_frames_fed_one_byte_at_a_time() {
  uint8_t stream[64];
  WlPong p;
  p.rxCount = 1;
  size_t n = wlEncodePong(stream, sizeof stream, p);
  WlQuiet q;
  q.on = 1;
  n += wlEncodeQuiet(stream + n, sizeof stream - n, q);

  WlDecoder dec;
  int frames = 0;
  uint8_t types[2] = {0, 0};
  for (size_t i = 0; i < n; i++) {
    TEST_ASSERT_EQUAL(1, dec.feed(stream + i, 1));
    while (dec.peek() == WlFeed::Frame) {
      types[frames++] = dec.type();
      dec.pop();
    }
  }
  TEST_ASSERT_EQUAL(2, frames);
  TEST_ASSERT_EQUAL((uint8_t)WlType::Pong, types[0]);
  TEST_ASSERT_EQUAL((uint8_t)WlType::Quiet, types[1]);
  TEST_ASSERT_EQUAL(0, dec.have);
}

static void test_decoder_takes_two_frames_from_one_read() {
  uint8_t stream[64];
  size_t n = wlEncodeEmpty(stream, sizeof stream, WlType::Ping);
  WlShown s;
  s.renderId = 9;
  n += wlEncodeShown(stream + n, sizeof stream - n, s);
  WlDecoder dec;
  TEST_ASSERT_EQUAL(n, dec.feed(stream, n));
  TEST_ASSERT_EQUAL(WlFeed::Frame, dec.peek());
  TEST_ASSERT_EQUAL(0, dec.payloadLen());
  dec.pop();
  TEST_ASSERT_EQUAL(WlFeed::Frame, dec.peek());
  WlShown d;
  TEST_ASSERT_TRUE(wlDecodeShown(dec.payload(), dec.payloadLen(), d));
  TEST_ASSERT_EQUAL_UINT32(9, d.renderId);
  dec.pop();
  TEST_ASSERT_EQUAL(WlFeed::NeedMore, dec.peek());
}

static void test_decoder_rejects_a_length_over_the_maximum() {
  const uint8_t bad[] = {0x02, 0x01, (uint8_t)WlType::Show};  // 513
  WlDecoder dec;
  dec.feed(bad, sizeof bad);
  TEST_ASSERT_EQUAL(WlFeed::Bad, dec.peek());
}

static void test_decoder_holds_a_frame_of_the_maximum_size() {
  static uint8_t big[WL_MAX_FRAME + 8];
  big[0] = (uint8_t)(WL_MAX_PAYLOAD >> 8);
  big[1] = (uint8_t)WL_MAX_PAYLOAD;
  big[2] = (uint8_t)WlType::LogLine;
  WlDecoder dec;
  // The buffer stops at one full frame; the rest waits for pop().
  TEST_ASSERT_EQUAL(WL_MAX_FRAME, dec.feed(big, sizeof big));
  TEST_ASSERT_EQUAL(WlFeed::Frame, dec.peek());
  dec.pop();
  TEST_ASSERT_EQUAL(0, dec.have);
}

static void test_decoder_passes_an_unknown_type_through() {
  const uint8_t unknown[] = {0x00, 0x01, 200, 0x55};
  WlDecoder dec;
  dec.feed(unknown, sizeof unknown);
  TEST_ASSERT_EQUAL(WlFeed::Frame, dec.peek());
  TEST_ASSERT_EQUAL(200, dec.type());
}

// ---- link clock ---------------------------------------------------------------------

static void test_clock_trusts_the_shortest_round_trip() {
  WlClockSync c;
  TEST_ASSERT_FALSE(c.valid());
  c.add(1000, 1300, 51150);  // 300 ms round trip
  c.add(2000, 2004, 52002);  // 4 ms: offset 50000
  c.add(3000, 3700, 53100);  // 700 ms
  TEST_ASSERT_TRUE(c.valid());
  TEST_ASSERT_EQUAL_INT32(50000, c.offsetMs());
  TEST_ASSERT_EQUAL_UINT32(10000, c.toRowMs(60000));
  TEST_ASSERT_EQUAL_UINT32(60000, c.toMasterMs(10000));
}

static void test_clock_survives_the_millisecond_counter_wrapping() {
  WlClockSync c;
  // Row clock just before its wrap, master clock just after its own.
  c.add(0xFFFFFFF0u, 0xFFFFFFF4u, 0x00000010u);
  const uint32_t at = c.toRowMs(0x00000100u);
  TEST_ASSERT_EQUAL_INT32(0xEE, wlMsUntil(at, 0xFFFFFFF4u));
  TEST_ASSERT_TRUE(wlMsUntil(at, at + 5) < 0);  // already past: flip now
}

static void test_clock_forgets_samples_older_than_its_window() {
  WlClockSync c;
  c.add(0, 1, 1000);  // a perfect sample of an offset that then changes
  for (uint32_t i = 1; i <= WL_CLOCK_SAMPLES; i++) c.add(i * 100, i * 100 + 10, i * 100 + 5 + 5000);
  TEST_ASSERT_EQUAL_INT32(5000, c.offsetMs());
}

// The round trips recorded on the live row: single ones reach hundreds of ms,
// the estimate holds to a few.
static void test_clock_on_the_recorded_round_trips() {
  const size_t n = sizeof(ROUND_TRIPS_560) / sizeof(ROUND_TRIPS_560[0]);
  WlClockSync c;
  int32_t lo = 0, hi = 0, rawLo = 0, rawHi = 0;
  bool first = true;
  for (size_t i = 0; i < n; i++) {
    const RoundTrip& r = ROUND_TRIPS_560[i];
    c.add(r.sentMs, r.sentMs + r.rttMs, r.peerMs);
    const int32_t raw = (int32_t)(r.peerMs - (r.sentMs + r.rttMs / 2));
    if (i == 0 || raw < rawLo) rawLo = raw;
    if (i == 0 || raw > rawHi) rawHi = raw;
    if (i + 1 < WL_CLOCK_SAMPLES) continue;
    const int32_t est = c.offsetMs();
    if (first || est < lo) lo = est;
    if (first || est > hi) hi = est;
    first = false;
  }
  TEST_ASSERT_TRUE(n >= 400);
  TEST_ASSERT_TRUE(rawHi - rawLo > 100);
  TEST_ASSERT_TRUE(hi - lo <= 4);
}

// ---- pacing ---------------------------------------------------------------------------

static void test_pacer_stops_at_four_unanswered_frames() {
  WlPacer p;
  for (int i = 0; i < WL_MAX_UNANSWERED; i++) {
    TEST_ASSERT_TRUE(p.canSend());
    p.onSent();
  }
  TEST_ASSERT_FALSE(p.canSend());
  p.onRxCount(1);
  TEST_ASSERT_TRUE(p.canSend());
  TEST_ASSERT_EQUAL(3, p.outstanding());
  p.onRxCount(4);
  TEST_ASSERT_EQUAL(0, p.outstanding());
}

static void test_pacer_ignores_a_count_that_goes_backwards_or_ahead() {
  WlPacer p;
  for (int i = 0; i < 3; i++) p.onSent();
  p.onRxCount(2);
  p.onRxCount(1);   // an older frame that crossed
  TEST_ASSERT_EQUAL(1, p.outstanding());
  p.onRxCount(9);   // more than was ever sent
  TEST_ASSERT_EQUAL(1, p.outstanding());
}

static void test_pacer_counts_across_the_counter_wrap() {
  WlPacer p;
  p.sent = 65534;
  p.handled = 65534;
  p.onSent();
  p.onSent();
  p.onSent();  // sent == 1
  TEST_ASSERT_EQUAL(3, p.outstanding());
  p.onRxCount(0);
  TEST_ASSERT_EQUAL(1, p.outstanding());
}

static void test_pacer_holds_pings_while_a_job_runs_or_frames_are_out() {
  WlPacer p;
  TEST_ASSERT_TRUE(p.canPing(false));
  TEST_ASSERT_FALSE(p.canPing(true));
  p.onSent();
  TEST_ASSERT_FALSE(p.canPing(false));
  p.onConnect();
  TEST_ASSERT_TRUE(p.canPing(false));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_hello);
  RUN_TEST(test_welcome);
  RUN_TEST(test_time_request_and_reply);
  RUN_TEST(test_show_and_shown);
  RUN_TEST(test_quiet_config_logctl);
  RUN_TEST(test_op_and_its_state);
  RUN_TEST(test_update);
  RUN_TEST(test_status_event_logline_pong);
  RUN_TEST(test_empty_messages);
  RUN_TEST(test_a_field_over_its_limit_fails_the_frame);
  RUN_TEST(test_a_frame_that_does_not_fit_is_not_built);
  RUN_TEST(test_a_cut_message_fails_to_decode);
  RUN_TEST(test_a_message_may_grow_at_its_end);
  RUN_TEST(test_op_state_data_cannot_reach_past_the_payload);
  RUN_TEST(test_master_and_row_types_do_not_overlap);
  RUN_TEST(test_decoder_reassembles_frames_fed_one_byte_at_a_time);
  RUN_TEST(test_decoder_takes_two_frames_from_one_read);
  RUN_TEST(test_decoder_rejects_a_length_over_the_maximum);
  RUN_TEST(test_decoder_holds_a_frame_of_the_maximum_size);
  RUN_TEST(test_decoder_passes_an_unknown_type_through);
  RUN_TEST(test_clock_trusts_the_shortest_round_trip);
  RUN_TEST(test_clock_survives_the_millisecond_counter_wrapping);
  RUN_TEST(test_clock_forgets_samples_older_than_its_window);
  RUN_TEST(test_clock_on_the_recorded_round_trips);
  RUN_TEST(test_pacer_stops_at_four_unanswered_frames);
  RUN_TEST(test_pacer_ignores_a_count_that_goes_backwards_or_ahead);
  RUN_TEST(test_pacer_counts_across_the_counter_wrap);
  RUN_TEST(test_pacer_holds_pings_while_a_job_runs_or_frames_are_out);
  return UNITY_END();
}
