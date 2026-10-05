// WallLink.h — the link between a Split-Flap's master (ESP32-S3) and its row
// boards (ESP-01): frame format, messages, link clock and pacing. Pure C++,
// compiled by the Master and FollowerEsp01 trees and their native tests.
// Spec: docs/superpowers/specs/2026-10-05-wall-link-and-console-design.md.
//
// One TCP connection per row board, opened by the row. A frame is
//   payload length u16 | type u8 | payload (at most WL_MAX_PAYLOAD)
// and every multi-byte field is big-endian. Strings carry a u8 length and no
// terminator on the wire.
//
// Compatibility rule: a message only ever grows at its end. A decoder reads
// the fields it knows and ignores what follows; a type it does not know is
// skipped by the caller. A length above WL_MAX_PAYLOAD is the one malformed
// frame, and it ends the connection.
//
// Lives outside shared/ on purpose: no unit compiles it, so an edit here must
// not move the unit source head.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

constexpr uint8_t WALL_LINK_PROTOCOL = 1;
constexpr uint16_t WALL_LINK_PORT = 7411;

constexpr size_t WL_HEADER_LEN = 3;
constexpr size_t WL_MAX_PAYLOAD = 512;
constexpr size_t WL_MAX_FRAME = WL_HEADER_LEN + WL_MAX_PAYLOAD;

constexpr size_t WL_ID_MAX = 32;    // device name, as DeviceIdentity builds it
constexpr size_t WL_REV_MAX = 16;   // short SHA, optionally "-dirty"
constexpr size_t WL_TZ_MAX = 64;    // POSIX tz rule
constexpr size_t WL_TEXT_MAX = 16;  // one row's flaps
constexpr size_t WL_OP_ARGS_MAX = 16;
constexpr size_t WL_OP_DATA_MAX = 480;
constexpr size_t WL_LOG_MAX = 256;

// Master → row types are below 32, row → master types from 32 up.
enum class WlType : uint8_t {
  Welcome = 1,
  Time = 2,
  Show = 3,
  Quiet = 4,
  Config = 5,
  Op = 6,
  Update = 7,
  LogCtl = 8,
  Ping = 9,
  Restart = 10,
  Release = 11,

  Hello = 32,
  TimeReq = 33,
  Status = 34,
  Shown = 35,
  OpState = 36,
  Event = 37,
  LogLine = 38,
  Pong = 39,
};

inline bool wlTypeFromMaster(uint8_t type) { return type < 32; }

// ---- field writer / reader ---------------------------------------------------

struct WlWriter {
  uint8_t* p;
  size_t cap;
  size_t len = 0;
  bool ok = true;

  WlWriter(uint8_t* buf, size_t capacity) : p(buf), cap(capacity) {}

  void bytes(const void* src, size_t n) {
    if (!ok || n > cap - len) {
      ok = false;
      return;
    }
    memcpy(p + len, src, n);
    len += n;
  }
  void u8(uint8_t v) { bytes(&v, 1); }
  void u16(uint16_t v) {
    const uint8_t b[2] = {(uint8_t)(v >> 8), (uint8_t)v};
    bytes(b, 2);
  }
  void u32(uint32_t v) {
    const uint8_t b[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
    bytes(b, 4);
  }
  // A string longer than its field's limit fails the frame; it is never cut.
  void str(const char* s, size_t maxLen) {
    const size_t n = strlen(s);
    if (n > maxLen || n > 255) {
      ok = false;
      return;
    }
    u8((uint8_t)n);
    bytes(s, n);
  }
};

struct WlReader {
  const uint8_t* p;
  size_t len;
  size_t pos = 0;
  bool ok = true;

  WlReader(const uint8_t* payload, size_t payloadLen) : p(payload), len(payloadLen) {}

  bool bytes(void* dst, size_t n) {
    if (!ok || n > len - pos) {
      ok = false;
      return false;
    }
    memcpy(dst, p + pos, n);
    pos += n;
    return true;
  }
  uint8_t u8() {
    uint8_t v = 0;
    bytes(&v, 1);
    return v;
  }
  uint16_t u16() {
    uint8_t b[2] = {0, 0};
    bytes(b, 2);
    return (uint16_t)((b[0] << 8) | b[1]);
  }
  uint32_t u32() {
    uint8_t b[4] = {0, 0, 0, 0};
    bytes(b, 4);
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
  }
  // out holds maxLen + 1 bytes. A string over the field's limit fails the read.
  void str(char* out, size_t maxLen) {
    out[0] = 0;
    const uint8_t n = u8();
    if (!ok) return;
    if (n > maxLen) {
      ok = false;
      return;
    }
    if (bytes(out, n)) out[n] = 0;
  }
  size_t remaining() const { return ok ? len - pos : 0; }
};

// Builds one frame into `frame`; returns its total length, 0 when it does not
// fit or a field was out of range.
template <typename Fill>
inline size_t wlBuild(uint8_t* frame, size_t cap, WlType type, Fill fill) {
  if (cap < WL_HEADER_LEN) return 0;
  const size_t room = cap - WL_HEADER_LEN;
  WlWriter w(frame + WL_HEADER_LEN, room < WL_MAX_PAYLOAD ? room : WL_MAX_PAYLOAD);
  fill(w);
  if (!w.ok) return 0;
  frame[0] = (uint8_t)(w.len >> 8);
  frame[1] = (uint8_t)w.len;
  frame[2] = (uint8_t)type;
  return WL_HEADER_LEN + w.len;
}

// ---- stream decoder ----------------------------------------------------------

enum class WlFeed : uint8_t { NeedMore, Frame, Bad };

// Reassembles frames from a TCP byte stream. feed() what arrived, then while
// peek() says Frame: handle type()/payload()/payloadLen() and pop().
struct WlDecoder {
  uint8_t buf[WL_MAX_FRAME];
  size_t have = 0;

  // Returns how many of the bytes were taken (less than n when the buffer is
  // full, i.e. a frame is waiting to be popped).
  size_t feed(const uint8_t* data, size_t n) {
    const size_t room = sizeof(buf) - have;
    const size_t take = n < room ? n : room;
    memcpy(buf + have, data, take);
    have += take;
    return take;
  }
  size_t payloadLen() const { return ((size_t)buf[0] << 8) | buf[1]; }
  uint8_t type() const { return buf[2]; }
  const uint8_t* payload() const { return buf + WL_HEADER_LEN; }

  WlFeed peek() const {
    if (have < WL_HEADER_LEN) return WlFeed::NeedMore;
    if (payloadLen() > WL_MAX_PAYLOAD) return WlFeed::Bad;
    return have >= WL_HEADER_LEN + payloadLen() ? WlFeed::Frame : WlFeed::NeedMore;
  }
  void pop() {
    const size_t used = WL_HEADER_LEN + payloadLen();
    memmove(buf, buf + used, have - used);
    have -= used;
  }
  void reset() { have = 0; }
};

// ---- messages ----------------------------------------------------------------
//
// Every row → master message that the row sends after handling input carries
// `rxCount`: how many master frames this row has handled on this connection
// (wrapping). The master paces on it (WlPacer).

constexpr uint8_t WL_HELLO_RESCUE = 0x01;  // minimal boot after repeated early deaths

struct WlHello {
  uint8_t protocol = WALL_LINK_PROTOCOL;
  char id[WL_ID_MAX + 1] = "";
  char rev[WL_REV_MAX + 1] = "";
  uint32_t bootId = 0;  // changes at every start; open jobs die with the old one
  uint8_t flags = 0;
  uint8_t width = 0;    // units on this row
};

struct WlWelcome {
  uint8_t protocol = WALL_LINK_PROTOCOL;
  char masterId[WL_ID_MAX + 1] = "";
};

// Row asks, master answers; the round trip gives the row the master's clock.
struct WlTimeReq {
  uint32_t rowMs = 0;
  uint16_t rxCount = 0;
};

struct WlTime {
  uint32_t echoRowMs = 0;
  uint32_t masterMs = 0;   // link clock: the master's millis()
  uint32_t epochS = 0;     // wall clock, 0 while the master has no network time
};

struct WlShow {
  uint32_t renderId = 0;
  uint32_t atMs = 0;       // flip instant on the link clock
  uint8_t speed = 0;
  char text[WL_TEXT_MAX + 1] = "";
};

struct WlShown {
  uint32_t renderId = 0;
  uint16_t lateMs = 0;     // how far past atMs the frame was handled; 0 = on time
  uint16_t rxCount = 0;
};

struct WlQuiet {
  uint8_t on = 0;
};

enum class WlFallback : uint8_t { Blank = 0, Time = 1, Date = 2 };

struct WlConfig {
  uint8_t fallback = (uint8_t)WlFallback::Blank;  // what to show when the master is lost
  uint8_t updateUnitsAtStart = 1;
  char tz[WL_TZ_MAX + 1] = "";
};

struct WlOp {
  uint32_t opId = 0;       // named by the master
  uint8_t opcode = 0;
  uint8_t address = 0;     // unit bus address, 0 = the whole row
  uint8_t argsLen = 0;
  uint8_t args[WL_OP_ARGS_MAX] = {};
};

enum class WlOpPhase : uint8_t { Running = 0, Ok = 1, Failed = 2, Refused = 3 };

struct WlOpState {
  uint32_t opId = 0;
  uint8_t phase = 0;
  uint8_t reason = 0;      // MaintReason vocabulary
  uint16_t rxCount = 0;
  uint16_t dataOffset = 0; // for results sent in pieces (boot dump)
  uint16_t dataLen = 0;
  const uint8_t* data = nullptr;  // decode: points into the payload
};

struct WlUpdate {
  char rev[WL_REV_MAX + 1] = "";
  uint32_t size = 0;
  uint8_t md5[16] = {};
  uint8_t packed = 0;      // gzip image for eboot
};

struct WlLogCtl {
  uint8_t on = 0;
};

struct WlStatus {
  uint16_t rxCount = 0;
  uint32_t upS = 0;
  uint32_t heap = 0;
  uint32_t minHeap = 0;
  uint32_t maxBlock = 0;
  int8_t rssi = 0;
  uint8_t txPower = 0;     // quarter dBm
  uint32_t busTx = 0;
  uint32_t busErr = 0;
  uint8_t busDead = 0;
  uint16_t busEpisodes = 0;
  uint8_t escalations = 0;
  uint8_t jobRunning = 0;  // a unit job holds the loop: expect silence
  uint32_t imageSize = 0;
};

struct WlEvent {
  uint16_t code = 0;
  uint8_t unit = 0;
  uint32_t a = 0;
  uint32_t b = 0;
  uint32_t upS = 0;
};

struct WlPong {
  uint16_t rxCount = 0;
};

// -- encoders: return the frame length, 0 on a field out of range --

inline size_t wlEncodeHello(uint8_t* f, size_t cap, const WlHello& m) {
  return wlBuild(f, cap, WlType::Hello, [&](WlWriter& w) {
    w.u8(m.protocol); w.str(m.id, WL_ID_MAX); w.str(m.rev, WL_REV_MAX);
    w.u32(m.bootId); w.u8(m.flags); w.u8(m.width);
  });
}
inline size_t wlEncodeWelcome(uint8_t* f, size_t cap, const WlWelcome& m) {
  return wlBuild(f, cap, WlType::Welcome, [&](WlWriter& w) {
    w.u8(m.protocol); w.str(m.masterId, WL_ID_MAX);
  });
}
inline size_t wlEncodeTimeReq(uint8_t* f, size_t cap, const WlTimeReq& m) {
  return wlBuild(f, cap, WlType::TimeReq, [&](WlWriter& w) { w.u32(m.rowMs); w.u16(m.rxCount); });
}
inline size_t wlEncodeTime(uint8_t* f, size_t cap, const WlTime& m) {
  return wlBuild(f, cap, WlType::Time, [&](WlWriter& w) {
    w.u32(m.echoRowMs); w.u32(m.masterMs); w.u32(m.epochS);
  });
}
inline size_t wlEncodeShow(uint8_t* f, size_t cap, const WlShow& m) {
  return wlBuild(f, cap, WlType::Show, [&](WlWriter& w) {
    w.u32(m.renderId); w.u32(m.atMs); w.u8(m.speed); w.str(m.text, WL_TEXT_MAX);
  });
}
inline size_t wlEncodeShown(uint8_t* f, size_t cap, const WlShown& m) {
  return wlBuild(f, cap, WlType::Shown, [&](WlWriter& w) {
    w.u32(m.renderId); w.u16(m.lateMs); w.u16(m.rxCount);
  });
}
inline size_t wlEncodeQuiet(uint8_t* f, size_t cap, const WlQuiet& m) {
  return wlBuild(f, cap, WlType::Quiet, [&](WlWriter& w) { w.u8(m.on); });
}
inline size_t wlEncodeConfig(uint8_t* f, size_t cap, const WlConfig& m) {
  return wlBuild(f, cap, WlType::Config, [&](WlWriter& w) {
    w.u8(m.fallback); w.u8(m.updateUnitsAtStart); w.str(m.tz, WL_TZ_MAX);
  });
}
inline size_t wlEncodeOp(uint8_t* f, size_t cap, const WlOp& m) {
  return wlBuild(f, cap, WlType::Op, [&](WlWriter& w) {
    if (m.argsLen > WL_OP_ARGS_MAX) { w.ok = false; return; }
    w.u32(m.opId); w.u8(m.opcode); w.u8(m.address); w.u8(m.argsLen); w.bytes(m.args, m.argsLen);
  });
}
inline size_t wlEncodeOpState(uint8_t* f, size_t cap, const WlOpState& m) {
  return wlBuild(f, cap, WlType::OpState, [&](WlWriter& w) {
    if (m.dataLen > WL_OP_DATA_MAX || (m.dataLen && !m.data)) { w.ok = false; return; }
    w.u32(m.opId); w.u8(m.phase); w.u8(m.reason); w.u16(m.rxCount);
    w.u16(m.dataOffset); w.u16(m.dataLen); w.bytes(m.data, m.dataLen);
  });
}
inline size_t wlEncodeUpdate(uint8_t* f, size_t cap, const WlUpdate& m) {
  return wlBuild(f, cap, WlType::Update, [&](WlWriter& w) {
    w.str(m.rev, WL_REV_MAX); w.u32(m.size); w.bytes(m.md5, sizeof(m.md5)); w.u8(m.packed);
  });
}
inline size_t wlEncodeLogCtl(uint8_t* f, size_t cap, const WlLogCtl& m) {
  return wlBuild(f, cap, WlType::LogCtl, [&](WlWriter& w) { w.u8(m.on); });
}
inline size_t wlEncodeStatus(uint8_t* f, size_t cap, const WlStatus& m) {
  return wlBuild(f, cap, WlType::Status, [&](WlWriter& w) {
    w.u16(m.rxCount); w.u32(m.upS); w.u32(m.heap); w.u32(m.minHeap); w.u32(m.maxBlock);
    w.u8((uint8_t)m.rssi); w.u8(m.txPower); w.u32(m.busTx); w.u32(m.busErr);
    w.u8(m.busDead); w.u16(m.busEpisodes); w.u8(m.escalations); w.u8(m.jobRunning);
    w.u32(m.imageSize);
  });
}
inline size_t wlEncodeEvent(uint8_t* f, size_t cap, const WlEvent& m) {
  return wlBuild(f, cap, WlType::Event, [&](WlWriter& w) {
    w.u16(m.code); w.u8(m.unit); w.u32(m.a); w.u32(m.b); w.u32(m.upS);
  });
}
inline size_t wlEncodeLogLine(uint8_t* f, size_t cap, const char* text, size_t n) {
  return wlBuild(f, cap, WlType::LogLine, [&](WlWriter& w) {
    if (n > WL_LOG_MAX) { w.ok = false; return; }
    w.bytes(text, n);
  });
}
inline size_t wlEncodePong(uint8_t* f, size_t cap, const WlPong& m) {
  return wlBuild(f, cap, WlType::Pong, [&](WlWriter& w) { w.u16(m.rxCount); });
}
// Ping, Restart and Release carry nothing.
inline size_t wlEncodeEmpty(uint8_t* f, size_t cap, WlType type) {
  return wlBuild(f, cap, type, [](WlWriter&) {});
}

// -- decoders: false when a known field is missing or out of range --

inline bool wlDecodeHello(const uint8_t* p, size_t n, WlHello& m) {
  WlReader r(p, n);
  m.protocol = r.u8(); r.str(m.id, WL_ID_MAX); r.str(m.rev, WL_REV_MAX);
  m.bootId = r.u32(); m.flags = r.u8(); m.width = r.u8();
  return r.ok;
}
inline bool wlDecodeWelcome(const uint8_t* p, size_t n, WlWelcome& m) {
  WlReader r(p, n);
  m.protocol = r.u8(); r.str(m.masterId, WL_ID_MAX);
  return r.ok;
}
inline bool wlDecodeTimeReq(const uint8_t* p, size_t n, WlTimeReq& m) {
  WlReader r(p, n);
  m.rowMs = r.u32(); m.rxCount = r.u16();
  return r.ok;
}
inline bool wlDecodeTime(const uint8_t* p, size_t n, WlTime& m) {
  WlReader r(p, n);
  m.echoRowMs = r.u32(); m.masterMs = r.u32(); m.epochS = r.u32();
  return r.ok;
}
inline bool wlDecodeShow(const uint8_t* p, size_t n, WlShow& m) {
  WlReader r(p, n);
  m.renderId = r.u32(); m.atMs = r.u32(); m.speed = r.u8(); r.str(m.text, WL_TEXT_MAX);
  return r.ok;
}
inline bool wlDecodeShown(const uint8_t* p, size_t n, WlShown& m) {
  WlReader r(p, n);
  m.renderId = r.u32(); m.lateMs = r.u16(); m.rxCount = r.u16();
  return r.ok;
}
inline bool wlDecodeQuiet(const uint8_t* p, size_t n, WlQuiet& m) {
  WlReader r(p, n);
  m.on = r.u8();
  return r.ok;
}
inline bool wlDecodeConfig(const uint8_t* p, size_t n, WlConfig& m) {
  WlReader r(p, n);
  m.fallback = r.u8(); m.updateUnitsAtStart = r.u8(); r.str(m.tz, WL_TZ_MAX);
  return r.ok;
}
inline bool wlDecodeOp(const uint8_t* p, size_t n, WlOp& m) {
  WlReader r(p, n);
  m.opId = r.u32(); m.opcode = r.u8(); m.address = r.u8(); m.argsLen = r.u8();
  if (m.argsLen > WL_OP_ARGS_MAX) return false;
  r.bytes(m.args, m.argsLen);
  return r.ok;
}
inline bool wlDecodeOpState(const uint8_t* p, size_t n, WlOpState& m) {
  WlReader r(p, n);
  m.opId = r.u32(); m.phase = r.u8(); m.reason = r.u8(); m.rxCount = r.u16();
  m.dataOffset = r.u16(); m.dataLen = r.u16();
  if (!r.ok || m.dataLen > WL_OP_DATA_MAX || m.dataLen > r.remaining()) return false;
  m.data = m.dataLen ? p + r.pos : nullptr;
  return true;
}
inline bool wlDecodeUpdate(const uint8_t* p, size_t n, WlUpdate& m) {
  WlReader r(p, n);
  r.str(m.rev, WL_REV_MAX); m.size = r.u32(); r.bytes(m.md5, sizeof(m.md5)); m.packed = r.u8();
  return r.ok;
}
inline bool wlDecodeLogCtl(const uint8_t* p, size_t n, WlLogCtl& m) {
  WlReader r(p, n);
  m.on = r.u8();
  return r.ok;
}
inline bool wlDecodeStatus(const uint8_t* p, size_t n, WlStatus& m) {
  WlReader r(p, n);
  m.rxCount = r.u16(); m.upS = r.u32(); m.heap = r.u32(); m.minHeap = r.u32(); m.maxBlock = r.u32();
  m.rssi = (int8_t)r.u8(); m.txPower = r.u8(); m.busTx = r.u32(); m.busErr = r.u32();
  m.busDead = r.u8(); m.busEpisodes = r.u16(); m.escalations = r.u8(); m.jobRunning = r.u8();
  m.imageSize = r.u32();
  return r.ok;
}
inline bool wlDecodeEvent(const uint8_t* p, size_t n, WlEvent& m) {
  WlReader r(p, n);
  m.code = r.u16(); m.unit = r.u8(); m.a = r.u32(); m.b = r.u32(); m.upS = r.u32();
  return r.ok;
}
inline bool wlDecodePong(const uint8_t* p, size_t n, WlPong& m) {
  WlReader r(p, n);
  m.rxCount = r.u16();
  return r.ok;
}

// ---- link clock ---------------------------------------------------------------
//
// The row keeps the last WL_CLOCK_SAMPLES round trips and trusts the one with
// the shortest round trip: a slow frame says nothing about the clocks. Measured
// on the live row (#560): spread 0.35 ms, worst 0.9 ms; single round trips
// reached 772 ms.

constexpr uint8_t WL_CLOCK_SAMPLES = 25;

struct WlClockSync {
  struct Sample {
    int32_t offsetMs;   // master clock minus row clock
    uint16_t rttMs;
  };
  Sample s[WL_CLOCK_SAMPLES];
  uint8_t count = 0;
  uint8_t next = 0;

  // sentRowMs: the row's clock when it sent TimeReq (echoed back in Time);
  // nowRowMs: the row's clock when Time arrived.
  void add(uint32_t sentRowMs, uint32_t nowRowMs, uint32_t masterMs) {
    const uint32_t rtt = nowRowMs - sentRowMs;
    Sample& x = s[next];
    x.rttMs = rtt > 0xFFFF ? 0xFFFF : (uint16_t)rtt;
    x.offsetMs = (int32_t)(masterMs - (sentRowMs + rtt / 2));
    next = (uint8_t)((next + 1) % WL_CLOCK_SAMPLES);
    if (count < WL_CLOCK_SAMPLES) count++;
  }
  bool valid() const { return count > 0; }
  const Sample& best() const {
    uint8_t b = 0;
    for (uint8_t i = 1; i < count; i++)
      if (s[i].rttMs < s[b].rttMs) b = i;
    return s[b];
  }
  int32_t offsetMs() const { return best().offsetMs; }
  // A master link-clock instant on the row's own clock.
  uint32_t toRowMs(uint32_t masterMs) const { return masterMs - (uint32_t)offsetMs(); }
  uint32_t toMasterMs(uint32_t rowMs) const { return rowMs + (uint32_t)offsetMs(); }
  void reset() { count = 0; next = 0; }
};

// How long until a flip instant, on the row's clock; <= 0 means it has passed
// and the row flips now.
inline int32_t wlMsUntil(uint32_t atRowMs, uint32_t nowRowMs) { return (int32_t)(atRowMs - nowRowMs); }

// ---- pacing (master side) -------------------------------------------------------
//
// A row in a unit job does not read its socket (measured: up to 12.4 s) and
// every unread frame costs it about 80 B. The master therefore never has more
// than WL_MAX_UNANSWERED frames out to one row, and holds pings while a job
// runs there.

constexpr uint16_t WL_MAX_UNANSWERED = 4;

struct WlPacer {
  uint16_t sent = 0;      // frames written to this row on this connection
  uint16_t handled = 0;   // the row's latest rxCount

  void onConnect() { sent = 0; handled = 0; }
  void onSent() { sent++; }
  // Row counters only move forward; a stale value (frames crossing) is ignored.
  void onRxCount(uint16_t rxCount) {
    if ((uint16_t)(rxCount - handled) <= (uint16_t)(sent - handled)) handled = rxCount;
  }
  uint16_t outstanding() const { return (uint16_t)(sent - handled); }
  bool canSend() const { return outstanding() < WL_MAX_UNANSWERED; }
  bool canPing(bool rowJobRunning) const { return !rowJobRunning && outstanding() == 0; }
};
