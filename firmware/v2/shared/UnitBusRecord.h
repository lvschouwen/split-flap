#pragma once
// Pure logic for the SFP_CMD_GET_BUS_RECORD reply (#584): what a unit
// remembers, across power cycles, about the times its master stopped
// addressing it. Natively tested by the Unit's test_bus_record. The rules that
// fill it are the unit's UnitBusSilence.h; the EEPROM block is UnitEeprom.h.
//
// Why the unit keeps it: a row whose units stop answering with both bus lines
// free (#496) looks the same from the row board whether the wire is off, the
// units are deaf, or only the way back is broken. The units stay powered
// through it, and everything they know in RAM goes with the power cycle that
// ends it. Read after the row is back:
//   - no silence recorded          the unit was written to all along: the way
//                                  back (its replies) is what failed
//   - silence, traffic seen        the lines reach the unit and carry traffic,
//                                  but nothing was addressed to it
//   - silence, no traffic seen     nothing arrived at the unit at all: the
//                                  wire, or the board that drives it
//
// Wire format (9 bytes):
//   off  field          type  notes
//   0    silences       u8    times contact stopped, saturating: every one
//                             since the unit started, and of the earlier ones
//                             those that lasted long enough to be written
//   1    lastMinutes    u8    how long the latest one lasted, saturating; a
//                             silence a reset ended reads the last mark saved
//   2    longestMinutes u8    the longest of them
//   3    reinits        u8    bus hardware restarts the unit tried on silence
//   4    reinitsHeard   u8    of those, the ones contact came back right after
//   5    selfRestarts   u8    whole-unit restarts on silence
//   6    flags          u8    of the latest silence, BUS_RECORD_FLAG_*
//   7    silentNow      u8    minutes of the silence still going on, 0 = none;
//                             since boot, not stored
//   8    checksum       u8    XOR of 0..7 ^ the mask below
//
// A unit predating the opcode answers its 1-byte rotation status plus bus
// padding: the length check and the masked checksum reject every such shape,
// and the reader keeps "no record" (the #231/#106 pattern).

#include <stdint.h>

#define BUS_RECORD_REPLY_LEN           9
#define BUS_RECORD_REPLY_CHECKSUM_MASK 0xD4

// Traffic was seen on the lines while nothing was addressed to this unit.
#define BUS_RECORD_FLAG_TRAFFIC   0x01
// A line read low when the silence was noticed.
#define BUS_RECORD_FLAG_LINE_LOW  0x02
// It ended with the master addressing the unit again. Clear: it was still
// going on when the unit was reset or lost power.
#define BUS_RECORD_FLAG_ENDED     0x04
// The unit restarted itself in it.
#define BUS_RECORD_FLAG_RESTARTED 0x08

struct UnitBusRecord {
  uint8_t silences = 0;
  uint8_t lastMinutes = 0;
  uint8_t longestMinutes = 0;
  uint8_t reinits = 0;
  uint8_t reinitsHeard = 0;
  uint8_t selfRestarts = 0;
  uint8_t flags = 0;
};

inline uint8_t busRecordChecksum(const uint8_t buf[BUS_RECORD_REPLY_LEN]) {
  uint8_t x = 0;
  for (uint8_t i = 0; i < BUS_RECORD_REPLY_LEN - 1; i++) x ^= buf[i];
  return (uint8_t)(x ^ BUS_RECORD_REPLY_CHECKSUM_MASK);
}

inline void busRecordEncodeReply(const UnitBusRecord& r, uint8_t silentNowMinutes,
                                 uint8_t buf[BUS_RECORD_REPLY_LEN]) {
  buf[0] = r.silences;
  buf[1] = r.lastMinutes;
  buf[2] = r.longestMinutes;
  buf[3] = r.reinits;
  buf[4] = r.reinitsHeard;
  buf[5] = r.selfRestarts;
  buf[6] = r.flags;
  buf[7] = silentNowMinutes;
  buf[8] = busRecordChecksum(buf);
}

// `len` is what the transaction returned. `out` and `silentNowMinutes` are
// left untouched on rejection.
inline bool busRecordReadbackValid(const uint8_t* buf, uint8_t len, UnitBusRecord& out,
                                   uint8_t& silentNowMinutes) {
  if (len < BUS_RECORD_REPLY_LEN) return false;
  if (buf[BUS_RECORD_REPLY_LEN - 1] != busRecordChecksum(buf)) return false;
  out.silences = buf[0];
  out.lastMinutes = buf[1];
  out.longestMinutes = buf[2];
  out.reinits = buf[3];
  out.reinitsHeard = buf[4];
  out.selfRestarts = buf[5];
  out.flags = buf[6];
  silentNowMinutes = buf[7];
  return true;
}
