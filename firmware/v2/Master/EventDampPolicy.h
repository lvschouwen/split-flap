#pragma once
// EventDampPolicy.h — the same entry over and over is written a few times, then
// counted (#571). Pure, natively tested by test_event_damp; run by
// EventRecord.cpp on netTask as entries are written.
//
// Only an entry identical in everything but its time and seq is damped: the
// kind, the board, the unit, the detail and both numbers. Of those, the first
// EVENT_DAMP_BURST in EVENT_DAMP_WINDOW_S are written; the rest are counted,
// and once that window has passed one Repeated entry says how many were not
// written — the kernel's printk ratelimit (a burst of 10 a window, then
// "N suppressed") in rsyslog's form ("message repeated N times"). The window
// is an hour, not the kernel's 5 s: a board that restarts over and over adds
// an entry every half minute, and on 2026-10-19 a master in a restart loop
// wrote a third of the record's 13 days in two hours.
//
// The table is plain data so it can live in RTC memory and survive a restart
// that is not a power cut (EventRecord.cpp) — the master's own start is what
// repeats most. After a power cut it is rebuilt from the record's newest
// entries, without the counts of what was not written then.

#include <stdint.h>
#include <string.h>

#include <type_traits>

#include "EventRecordPolicy.h"

#define EVENT_DAMP_BURST 10
#define EVENT_DAMP_WINDOW_S 3600UL
// Distinct entries followed at once; the oldest window makes room.
#define EVENT_DAMP_SLOTS 16

// One entry as it repeats; kind 0 = the slot is free.
struct EventDampSlot {
  uint32_t a;
  uint32_t b;
  uint32_t windowStartS;
  uint32_t lastS;      // the newest one not written
  uint16_t board;
  uint16_t written;    // in this window
  uint16_t unwritten;  // in this window
  uint8_t kind;
  uint8_t detail;
  uint8_t unit;
};

// "EPMD" with the slot's size: a table an image of another layout left is not read.
#define EVENT_DAMP_MAGIC (0x444D5045UL ^ (uint32_t)sizeof(EventDampSlot))

// No default member initializers: it must stay trivially constructible for
// RTC_NOINIT. clear() or valid() before use.
struct EventDamp {
  uint32_t magic;
  EventDampSlot slots[EVENT_DAMP_SLOTS];
  uint32_t check;

  uint32_t sum() const {
    uint32_t h = 2166136261UL;
    const uint8_t* p = (const uint8_t*)slots;
    for (size_t i = 0; i < sizeof(slots); i++) {
      h ^= p[i];
      h *= 16777619UL;
    }
    return h;
  }
  bool valid() const { return magic == EVENT_DAMP_MAGIC && check == sum(); }
  void seal() {
    magic = EVENT_DAMP_MAGIC;
    check = sum();
  }
  void clear() {
    memset(slots, 0, sizeof(slots));
    seal();
  }

  static bool same(const EventDampSlot& s, const EventRecord& r) {
    return s.kind == r.kind && s.detail == r.detail && s.board == r.board && s.unit == r.unit &&
           s.a == r.a && s.b == r.b;
  }

  // The entry that says what a slot did not write, dated by the last of them.
  static EventRecord repeated(const EventDampSlot& s) {
    EventRecord r;
    r.kind = (uint8_t)EventKind::Repeated;
    r.detail = s.kind;
    r.board = s.board;
    r.unit = s.unit;
    r.a = s.unwritten;
    r.b = s.detail;
    r.timeS = s.lastS;
    return r;
  }

  EventDampSlot* find(const EventRecord& r) {
    for (EventDampSlot& s : slots) {
      if (s.kind != 0 && same(s, r)) return &s;
    }
    return nullptr;
  }

  // A free slot, else the one whose window began first.
  EventDampSlot* room() {
    EventDampSlot* oldest = &slots[0];
    for (EventDampSlot& s : slots) {
      if (s.kind == 0) return &s;
      if (s.windowStartS < oldest->windowStartS) oldest = &s;
    }
    return oldest;
  }

  // Whether `r` (with its time) is written. When its slot's window starts
  // over, or a slot is taken from another entry, and some were not written,
  // the Repeated entry for them goes to `summary` (`hasSummary`), to be
  // written before `r`. An entry without a time is always written: there is
  // no window to place it in.
  bool admit(const EventRecord& r, EventRecord& summary, bool& hasSummary) {
    hasSummary = false;
    if (r.timeS == 0 || r.kind == (uint8_t)EventKind::Repeated) return true;
    EventDampSlot* slot = find(r);
    if (slot != nullptr && r.timeS >= slot->windowStartS &&
        r.timeS - slot->windowStartS < EVENT_DAMP_WINDOW_S) {
      if (slot->written < EVENT_DAMP_BURST) {
        slot->written++;
        seal();
        return true;
      }
      if (slot->unwritten < 0xFFFF) slot->unwritten++;
      slot->lastS = r.timeS;
      seal();
      return false;
    }
    if (slot == nullptr) slot = room();
    if (slot->kind != 0 && slot->unwritten > 0) {
      summary = repeated(*slot);
      hasSummary = true;
    }
    slot->kind = r.kind;
    slot->detail = r.detail;
    slot->board = r.board;
    slot->unit = r.unit;
    slot->a = r.a;
    slot->b = r.b;
    slot->windowStartS = r.timeS;
    slot->lastS = 0;
    slot->written = 1;
    slot->unwritten = 0;
    seal();
    return true;
  }

  // An entry the record already holds, read back after a power cut: it opens
  // or fills its window as written and is never counted as not written.
  void seed(const EventRecord& r) {
    if (r.timeS == 0 || r.kind == (uint8_t)EventKind::Repeated) return;
    EventDampSlot* slot = find(r);
    if (slot != nullptr && r.timeS >= slot->windowStartS &&
        r.timeS - slot->windowStartS < EVENT_DAMP_WINDOW_S) {
      if (slot->written < 0xFFFF) slot->written++;
      seal();
      return;
    }
    EventRecord ignored;
    bool hasSummary;
    admit(r, ignored, hasSummary);
  }

  // The Repeated entries of the windows that have passed by `nowS`, at most
  // `cap`; their slots are freed. Returns how many.
  int due(uint32_t nowS, EventRecord* out, int cap) {
    int n = 0;
    for (EventDampSlot& s : slots) {
      if (s.kind == 0 || nowS < s.windowStartS || nowS - s.windowStartS < EVENT_DAMP_WINDOW_S) {
        continue;
      }
      if (s.unwritten > 0) {
        if (n >= cap) break;
        out[n++] = repeated(s);
      }
      memset(&s, 0, sizeof(s));
    }
    seal();
    return n;
  }
};

static_assert(std::is_trivially_default_constructible<EventDamp>::value,
              "static init must not touch the RTC_NOINIT damp table");
