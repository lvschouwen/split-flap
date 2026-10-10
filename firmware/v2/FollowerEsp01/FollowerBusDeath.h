#pragma once
// FollowerBusDeath.h — what this row writes down at the moment its unit bus
// goes dead, and which units count as there (#496). Pure, natively tested by
// test_follower_bus_death; the Wire calls, the line test and the log text are
// FollowerBus.cpp and FollowerLineTest.cpp.
//
// A dead bus with both lines free reads the same from here whether the cable
// is off, the units are deaf or only their replies are lost. What tells them
// apart is taken once per episode, while it is fresh, and sent to the master
// as two events (RowEventCode in wall_link.proto):
//
//   ROW_EVT_BUS_DEAD   a = line state (Wire.status(), bits 0-3)
//                          (the three counts only with both lines free)
//                          | addresses that acknowledged   << 4   (5 bits)
//                          | addresses that did not        << 9   (5 bits)
//                          | addresses the bus refused     << 14  (5 bits)
//                          | units that had answered before << 19 (5 bits)
//                      b = seconds since this row last started a move,
//                          FOLLOWER_BUS_DEATH_NO_MOVE when it has not moved
//   ROW_EVT_BUS_LINES  a = how long SDA | SCL << 16 took to rise after being
//                          let go, with only what is outside the chip pulling
//                          them up, in tenths of a microsecond
//                      b = the same two as last measured on a working bus,
//                          0 when there was no such measurement yet
// A rise of FOLLOWER_LINE_NO_RISE is a line that stayed low for a
// millisecond: nothing outside the chip pulls it up. 0 is not measured (the
// line was held low to begin with).

#include <stdint.h>
#include <stdio.h>

#include "UnitHealth.h"

#define FOLLOWER_BUS_DEATH_NO_MOVE 0xFFFFUL
#define FOLLOWER_LINE_NO_RISE      0xFFFF
#define FOLLOWER_LINE_NOT_MEASURED 0

// A unit that sent something that checked out: its bootloader's identity, a
// version, or a status read. An address that only acknowledges is not one —
// a floating line does that (the phantom at address 9 of 2026-10-10).
inline bool followerUnitHasAnswered(const UnitFacts& f) {
  return f.state == 2 || (f.state == 1 && (f.protocolKnown || f.statusValid));
}

// A row with none of them is a dead bus, whatever acknowledged at its scan:
// it is probed again until one answers.
inline int followerUnitsAnswering(const UnitFacts* facts, int count) {
  int n = 0;
  for (int i = 0; i < count; i++) {
    if (followerUnitHasAnswered(facts[i])) n++;
  }
  return n;
}

struct FollowerBusDeath {
  uint8_t lineState = 0;   // Wire.status(): 0 free, 1-2 SCL held, 3-4 SDA held
  uint8_t acked = 0;       // of every address a unit can have
  uint8_t notAcked = 0;
  uint8_t refused = 0;     // the bus would not start a transfer at all
  uint8_t hadAnswered = 0; // units known when the bus died
  uint32_t sinceMoveS = FOLLOWER_BUS_DEATH_NO_MOVE;
};

namespace followerbusdeath {
inline uint32_t five(uint32_t v) { return v > 31 ? 31 : v; }
}  // namespace followerbusdeath

inline uint32_t followerBusDeathA(const FollowerBusDeath& d) {
  using followerbusdeath::five;
  return (uint32_t)(d.lineState & 0x0F) | five(d.acked) << 4 | five(d.notAcked) << 9 |
         five(d.refused) << 14 | five(d.hadAnswered) << 19;
}

inline uint32_t followerBusDeathB(const FollowerBusDeath& d) {
  return d.sinceMoveS >= FOLLOWER_BUS_DEATH_NO_MOVE ? FOLLOWER_BUS_DEATH_NO_MOVE : d.sinceMoveS;
}

inline uint32_t followerLinesPack(uint16_t sda, uint16_t scl) {
  return (uint32_t)sda | (uint32_t)scl << 16;
}

// Cycles of the CPU clock to tenths of a microsecond; never 0, which stands
// for not measured.
inline uint16_t followerLineTenths(uint32_t cycles, uint32_t cpuMhz) {
  if (cpuMhz == 0) return FOLLOWER_LINE_NOT_MEASURED;
  const uint32_t tenths = cycles * 10UL / cpuMhz;
  if (tenths >= FOLLOWER_LINE_NO_RISE) return FOLLOWER_LINE_NO_RISE - 1;
  return tenths == 0 ? 1 : (uint16_t)tenths;
}

// "1.2 us", "no rise", "not measured".
inline void followerLineText(char* out, size_t cap, uint16_t tenths) {
  if (tenths == FOLLOWER_LINE_NOT_MEASURED) {
    snprintf(out, cap, "not measured");
  } else if (tenths == FOLLOWER_LINE_NO_RISE) {
    snprintf(out, cap, "no rise");
  } else {
    snprintf(out, cap, "%u.%u us", (unsigned)(tenths / 10), (unsigned)(tenths % 10));
  }
}
