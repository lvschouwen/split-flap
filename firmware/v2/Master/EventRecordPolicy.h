#pragma once
// EventRecordPolicy.h — the record of what happened on the wall (#559/#570):
// what one entry is, how it is held until the clock is known, and how the
// record is read back newest first. Pure, natively tested by
// test_event_record; the files and the tasks are EventRecord.cpp.
//
// An entry is 24 bytes, little-endian:
//   0  u32 time     Unix seconds, 0 = the clock was not set
//   4  u32 seq      counts up for the life of the record, never 0
//   8  u32 a        what the two numbers mean depends on the kind
//   12 u32 b
//   16 u16 board    eventBoardKey() of the board's id, 0 = the master
//   18 u8  kind     EventKind
//   19 u8  detail   belongs to the kind (a reason, a job, a row's own code)
//   20 u8  unit     bus address, 0 = the board itself
//   21 u8  check    XOR of the other 23 bytes and EVENT_CHECK_MASK
//   22 u16 0
// A kind's number and the meaning of its fields never change; a new kind
// gets a new number. Wording is the reader's.
//
// The record is two files, appended to and rotated at EVENT_FILE_RECORDS
// (the flash log's discipline). Entries are never rewritten in place: LittleFS
// copies the rest of a file when its middle changes.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define EVENT_RECORD_SIZE 24
#define EVENT_CHECK_MASK 0x5A
// 49 KB a file; the record holds between one and two files' worth.
#define EVENT_FILE_RECORDS 2048
#define EVENT_STAGE_CAP 32
// How long an entry waits for the clock before it is written without a time.
// A start is recorded before the network is up; SNTP answers within seconds
// of the join, and the join window itself is 60 s (WifiPolicy.h).
#define EVENT_CLOCK_WAIT_MS 120000UL
#define EVENT_PAGE_MAX 50

//                                   detail          a                    b
enum class EventKind : uint8_t {
  MasterStarted = 1,       // reset reason   rev as a number
  EventsDropped = 2,       //                how many were lost
  UnitReasonOn = 10,       // UnitReason     the reason's numbers (UnitVerdict.h)
  UnitReasonOff = 11,      // UnitReason
  UnitRestarted = 12,      // reset cause    lifetime brownouts   lifetime watchdog resets
  BoardReasonOn = 20,      // BoardReason    the reason's numbers (BoardVerdict.h)
  BoardReasonOff = 21,     // BoardReason
  RowStarted = 22,         // 1 = rescue     rev as a number
  JobDone = 30,            // job number
  JobFailed = 31,          // job number
  RowEvent = 40,           // RowEventCode   its numbers (wall_link.proto)
};

inline const char* eventKindName(uint8_t kind) {
  switch ((EventKind)kind) {
    case EventKind::MasterStarted: return "master-started";
    case EventKind::EventsDropped: return "events-dropped";
    case EventKind::UnitReasonOn: return "unit-reason-on";
    case EventKind::UnitReasonOff: return "unit-reason-off";
    case EventKind::UnitRestarted: return "unit-restarted";
    case EventKind::BoardReasonOn: return "board-reason-on";
    case EventKind::BoardReasonOff: return "board-reason-off";
    case EventKind::RowStarted: return "row-started";
    case EventKind::JobDone: return "job-done";
    case EventKind::JobFailed: return "job-failed";
    case EventKind::RowEvent: return "row-event";
  }
  return "?";
}

struct EventRecord {
  uint32_t timeS = 0;
  uint32_t seq = 0;
  uint32_t a = 0;
  uint32_t b = 0;
  uint16_t board = 0;
  uint8_t kind = 0;
  uint8_t detail = 0;
  uint8_t unit = 0;
};

namespace eventrecord {
inline void put32(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}
inline uint32_t get32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
inline uint8_t check(const uint8_t* p) {
  uint8_t x = EVENT_CHECK_MASK;
  for (int i = 0; i < EVENT_RECORD_SIZE; i++) {
    if (i != 21) x ^= p[i];
  }
  return x;
}
}  // namespace eventrecord

inline void eventEncode(const EventRecord& r, uint8_t out[EVENT_RECORD_SIZE]) {
  eventrecord::put32(out, r.timeS);
  eventrecord::put32(out + 4, r.seq);
  eventrecord::put32(out + 8, r.a);
  eventrecord::put32(out + 12, r.b);
  out[16] = (uint8_t)r.board;
  out[17] = (uint8_t)(r.board >> 8);
  out[18] = r.kind;
  out[19] = r.detail;
  out[20] = r.unit;
  out[22] = 0;
  out[23] = 0;
  out[21] = eventrecord::check(out);
}

// False for bytes that are not an entry: a torn write, or the padding that
// follows one. A whole entry never has seq 0, so an erased or zeroed stretch
// is refused too.
inline bool eventDecode(const uint8_t in[EVENT_RECORD_SIZE], EventRecord& r) {
  if (in[21] != eventrecord::check(in)) return false;
  r.timeS = eventrecord::get32(in);
  r.seq = eventrecord::get32(in + 4);
  r.a = eventrecord::get32(in + 8);
  r.b = eventrecord::get32(in + 12);
  r.board = (uint16_t)(in[16] | (in[17] << 8));
  r.kind = in[18];
  r.detail = in[19];
  r.unit = in[20];
  return r.seq != 0;
}

// How a board is named in an entry: 0 for the master ("" is its id in the
// rows table), otherwise a 16-bit fold of FNV-1a over the id, never 0. A
// board that has left the wall is then still told apart from the others.
inline uint16_t eventBoardKey(const char* id) {
  if (id == nullptr || id[0] == '\0') return 0;
  uint32_t h = 2166136261UL;
  for (const char* p = id; *p != '\0'; p++) {
    h ^= (uint8_t)*p;
    h *= 16777619UL;
  }
  const uint16_t key = (uint16_t)(h ^ (h >> 16));
  return key == 0 ? 1 : key;
}

// A rev ("97e2679", "97e2679-dirty") as the number its first hex digits
// spell, at most eight of them; 0 when it does not start with one.
inline uint32_t eventRevNumber(const char* rev) {
  uint32_t n = 0;
  if (rev == nullptr) return 0;
  for (int i = 0; i < 8; i++) {
    const char c = rev[i];
    uint32_t d;
    if (c >= '0' && c <= '9') d = (uint32_t)(c - '0');
    else if (c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A' + 10);
    else break;
    n = (n << 4) | d;
  }
  return n;
}

// How far back on the uptime clock an entry is placed that happened `agoS`
// ago. The clock wraps after 49 days, so what is older than a week is placed
// a week back: its order is kept and its time is not believed anyway.
#define EVENT_AGO_MAX_S (7UL * 24UL * 3600UL)
inline uint32_t eventAgoMs(uint32_t agoS) {
  return (agoS > EVENT_AGO_MAX_S ? EVENT_AGO_MAX_S : agoS) * 1000UL;
}

// ---- holding entries until they are written ---------------------------------------

// Entries wait here, in order, with the moment they happened on the uptime
// clock. They are written once the wall clock is known, which also gives the
// ones that happened before it their time.
struct EventStage {
  struct Pending {
    EventRecord record;
    uint32_t atMs = 0;
  };
  Pending q[EVENT_STAGE_CAP];
  uint8_t count = 0;
  uint32_t dropped = 0;

  // False when there is no room: the entry is lost and counted.
  bool put(const EventRecord& record, uint32_t nowMs) {
    if (count >= EVENT_STAGE_CAP) {
      if (dropped < 0xFFFFFFFFUL) dropped++;
      return false;
    }
    q[count].record = record;
    q[count].atMs = nowMs;
    count++;
    return true;
  }

  // Moves the entries that may be written now into `out`, oldest first, each
  // with its time and the next seq. With the clock set that is all of them;
  // without it, those that have waited EVENT_CLOCK_WAIT_MS (all of them when
  // `force`: the board is about to restart). Entries lost for want of room
  // are owned up to in one entry behind them.
  int take(uint32_t nowEpochS, bool clockSet, uint32_t nowMs, bool force, uint32_t& nextSeq,
           EventRecord* out, int cap) {
    int n = 0;
    while (n < count && n < cap) {
      const uint32_t ageMs = nowMs - q[n].atMs;
      if (!clockSet && !force && ageMs < EVENT_CLOCK_WAIT_MS) break;
      out[n] = q[n].record;
      const uint32_t ageS = ageMs / 1000UL;
      out[n].timeS = clockSet && nowEpochS > ageS ? nowEpochS - ageS : 0;
      out[n].seq = nextSeq++;
      n++;
    }
    for (int i = n; i < count; i++) q[i - n] = q[i];
    count = (uint8_t)(count - n);
    if (dropped > 0 && count == 0 && n < cap && (clockSet || force || n > 0)) {
      EventRecord lost;
      lost.kind = (uint8_t)EventKind::EventsDropped;
      lost.a = dropped;
      lost.timeS = clockSet ? nowEpochS : 0;
      lost.seq = nextSeq++;
      out[n++] = lost;
      dropped = 0;
    }
    return n;
  }
};

// ---- the files ------------------------------------------------------------------

// Bytes to append so the next entry starts on an entry boundary again, after
// a write that was cut short. What they complete fails its check.
inline size_t eventPadNeeded(size_t fileSize) {
  return (EVENT_RECORD_SIZE - fileSize % EVENT_RECORD_SIZE) % EVENT_RECORD_SIZE;
}

inline bool eventShouldRotate(size_t fileSize) {
  return fileSize >= (size_t)EVENT_FILE_RECORDS * EVENT_RECORD_SIZE;
}

// A file is anything with
//   size_t size();
//   bool read(size_t offset, uint8_t* buf, size_t n);

// The seq of the newest entry in a file, 0 when it holds none.
template <class File>
uint32_t eventLastSeq(File& file) {
  uint8_t raw[EVENT_RECORD_SIZE];
  EventRecord r;
  for (size_t i = file.size() / EVENT_RECORD_SIZE; i > 0; i--) {
    if (!file.read((i - 1) * EVENT_RECORD_SIZE, raw, sizeof(raw))) return 0;
    if (eventDecode(raw, r)) return r.seq;
  }
  return 0;
}

// Appends to `out` (which holds `have` already) the entries of a file with
// seq below `before`, newest first; `before` 0 = from the newest. Returns the
// new count, at most `cap`.
template <class File>
int eventPageRead(File& file, uint32_t before, EventRecord* out, int cap, int have) {
  const size_t total = file.size() / EVENT_RECORD_SIZE;
  if (total == 0 || have >= cap) return have;
  size_t next = total;  // one past the entry to read next
  if (before != 0) {
    // Entries are in seq order and a seq is used once, so the wanted entry
    // lies no later than its distance from the newest; padding only moves it
    // earlier. Starts the walk there instead of at the end.
    const uint32_t last = eventLastSeq(file);
    if (last == 0) return have;
    if (last >= before) {
      const uint32_t skip = last - before + 1;
      if (skip >= total) return have;
      next = total - skip;
    }
  }
  const size_t CHUNK = 16;
  uint8_t raw[CHUNK * EVENT_RECORD_SIZE];
  while (next > 0 && have < cap) {
    const size_t n = next < CHUNK ? next : CHUNK;
    const size_t first = next - n;
    if (!file.read(first * EVENT_RECORD_SIZE, raw, n * EVENT_RECORD_SIZE)) return have;
    for (size_t i = n; i > 0 && have < cap; i--) {
      EventRecord r;
      if (!eventDecode(raw + (i - 1) * EVENT_RECORD_SIZE, r)) continue;
      if (before != 0 && r.seq >= before) continue;
      out[have++] = r;
    }
    next = first;
  }
  return have;
}
