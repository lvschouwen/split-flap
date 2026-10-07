#pragma once
// FollowerEvents.h — what this row has to tell its master about itself
// (#559/#570): its start and why, a restart it forced on itself, memory
// running low. The master keeps these in its event record; the codes are
// RowEventCode in wall_link.proto. Pure, natively tested by
// test_follower_events; FollowerLink.cpp sends them.
//
// An event waits here until the link can carry it: a start is known long
// before the master is reached. The few places are for that, not for a
// history; when they are taken the newer event is the one that goes, because
// the start and its cause are what cannot be learned any other way.

#include <stdint.h>

#define FOLLOWER_EVENTS_CAP 4

struct FollowerEvent {
  uint8_t code = 0;
  uint8_t unit = 0;
  uint32_t a = 0;
  uint32_t b = 0;
  uint32_t atS = 0;  // uptime when it happened
};

struct FollowerEvents {
  FollowerEvent q[FOLLOWER_EVENTS_CAP];
  uint8_t count = 0;

  // False when there was no place.
  bool put(uint8_t code, uint8_t unit, uint32_t a, uint32_t b, uint32_t nowS) {
    if (count >= FOLLOWER_EVENTS_CAP) return false;
    FollowerEvent& e = q[count++];
    e.code = code;
    e.unit = unit;
    e.a = a;
    e.b = b;
    e.atS = nowS;
    return true;
  }

  // The oldest one waiting, nullptr when none. It stays until pop(): a
  // message that could not be written is sent again.
  const FollowerEvent* front() const { return count > 0 ? &q[0] : nullptr; }

  void pop() {
    if (count == 0) return;
    for (uint8_t i = 1; i < count; i++) q[i - 1] = q[i];
    count--;
  }
};

inline uint32_t followerEventAgeS(const FollowerEvent& e, uint32_t nowS) {
  return nowS >= e.atS ? nowS - e.atS : 0;
}

// The one queue of this board. loop() context only.
inline FollowerEvents& followerEvents() {
  static FollowerEvents events;
  return events;
}
