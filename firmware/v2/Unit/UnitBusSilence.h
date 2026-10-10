#pragma once
// Pure rules for a unit that stops being addressed (#584): when that counts
// as a silence, what the unit tries, and what it writes down. Natively tested
// by test_bus_silence; the AVR glue (EEPROM, the pin-change watch, the
// restart) is in Unit.ino and UnitI2CProtocol.ino (bench tier). What is
// written down, and how it reads, is shared/UnitBusRecord.h.
//
// A unit has no way in but its bus, so a unit that hears nothing can only
// help itself. The two heals of UnitTwiHeal.h act on what they can see: a
// line held low, a control register gone wrong. This acts on the symptom
// alone, when both look fine and still nothing arrives:
//   - after BUS_SILENCE_AFTER_MS without a frame, and every such span again,
//     the bus hardware is restarted (the same re-init as the heals; no flap
//     moves);
//   - after BUS_SILENCE_RESTART_MS, once per silence and only behind
//     UNIT_GATE_SILENCE_RESTART, the unit restarts itself. It comes back
//     unhomed and finds home, so this one moves the drum — which is why it is
//     gated and why it happens once: a unit that restarted has no contact to
//     lose until its master addresses it again.
// Neither helps when the wire is what failed; the record says which it was.
//
// A silence needs contact first: a unit nobody has addressed
// BUS_SILENCE_CONTACT_FRAMES times since its start is on a bench, or waiting
// for a master that is not there, and has nothing to recover from.
//
// Not every silence is a fault. A master that updates other units, or its
// own firmware, does not address this one for minutes. Those end by
// themselves and usually with traffic on the lines; the master's history says
// what it was doing.
//
// EEPROM wear: a silence is written only once it has lasted twice
// BUS_SILENCE_AFTER_MS, then at each doubling of its length in minutes, and
// when it ends — a dozen block writes for a day of it. A shorter one is
// counted until the unit restarts and never written, so a master that comes
// and goes every few minutes costs no write at all.

#include <stdint.h>

#include "UnitBusRecord.h"

// A row master asks one unit for its status every 3 s (HeartbeatPolicy.h), so
// a unit on a 16-wide row is addressed every 48 s: the span is more than
// three of those. The restart waits out what a master legitimately takes to
// update a whole row of units (16 minutes at the outside) or itself.
#define BUS_SILENCE_AFTER_MS        180000UL
#define BUS_SILENCE_RESTART_MS      1200000UL
#define BUS_SILENCE_CONTACT_FRAMES  8
// Contact this soon after a bus hardware restart is counted as following it.
// It can also be the master coming back by itself: read the count next to the
// master's own log, not as proof.
#define BUS_SILENCE_REINIT_HEARD_MS 5000UL
// Traffic on the lines that is followed this closely by a frame for this unit
// was that frame arriving.
#define BUS_SILENCE_TRAFFIC_OWN_MS  1000UL

// What the glue does after a tick, any of them together.
#define BUS_SILENCE_DO_SAVE          0x01  // write the record to EEPROM
#define BUS_SILENCE_DO_REINIT        0x02  // restart the bus hardware
#define BUS_SILENCE_DO_WATCH_TRAFFIC 0x04  // arm the one-shot watch on the lines
#define BUS_SILENCE_DO_RESTART       0x08  // save was asked too: then restart

struct BusSilence {
  uint16_t lastFrames = 0;
  uint16_t contactFrames = 0;  // saturating at BUS_SILENCE_CONTACT_FRAMES
  uint32_t heardMs = 0;        // when a frame last arrived (or the unit started)
  bool     silent = false;
  uint8_t  savedMinutes = 0;   // the length last written, 0 = not written yet
  uint32_t reinitMs = 0;       // the last bus hardware restart
  bool     reinitOpen = false; // one was done and contact has not come back since
  bool     restarted = false;  // this silence has had its restart
  bool     trafficOpen = false;
  uint32_t trafficMs = 0;
};

inline uint8_t busSilenceMinutes(uint32_t ms) {
  const uint32_t minutes = ms / 60000UL;
  return minutes > 0xFF ? 0xFF : (uint8_t)minutes;
}

inline void busSilenceBump(uint8_t& counter) {
  if (counter < 0xFF) counter++;
}

// Minutes of the silence going on now, 0 when there is none.
inline uint8_t busSilenceNowMinutes(const BusSilence& s, uint32_t nowMs) {
  if (!s.silent) return 0;
  const uint8_t minutes = busSilenceMinutes(nowMs - s.heardMs);
  return minutes == 0 ? 1 : minutes;
}

// One pass of loop(). `frames`: the count of frames addressed to this unit
// (wrapping). `lineLow`: a bus line reads low now. `traffic`: the watch on
// the lines fired since the last pass. `restartAllowed`: the gate is on.
inline uint8_t busSilenceTick(BusSilence& s, UnitBusRecord& record, uint16_t frames, bool lineLow,
                              bool traffic, bool restartAllowed, uint32_t nowMs) {
  uint8_t todo = 0;
  if (frames != s.lastFrames) {
    const uint16_t arrived = (uint16_t)(frames - s.lastFrames);
    s.lastFrames = frames;
    s.contactFrames = (uint16_t)(s.contactFrames + arrived) > BUS_SILENCE_CONTACT_FRAMES
                          ? BUS_SILENCE_CONTACT_FRAMES
                          : (uint16_t)(s.contactFrames + arrived);
    if (s.silent) {
      const uint8_t minutes = busSilenceMinutes(nowMs - s.heardMs);
      record.lastMinutes = minutes == 0 ? 1 : minutes;
      if (record.lastMinutes > record.longestMinutes) record.longestMinutes = record.lastMinutes;
      record.flags |= BUS_RECORD_FLAG_ENDED;
      if (s.reinitOpen && nowMs - s.reinitMs <= BUS_SILENCE_REINIT_HEARD_MS) {
        busSilenceBump(record.reinitsHeard);
      }
      // One that was never written leaves nothing to finish in EEPROM.
      if (s.savedMinutes != 0) todo |= BUS_SILENCE_DO_SAVE;
    }
    s.heardMs = nowMs;
    s.silent = false;
    s.reinitOpen = false;
    s.restarted = false;
    s.trafficOpen = false;
    return todo;
  }
  if (s.contactFrames < BUS_SILENCE_CONTACT_FRAMES) {
    // Nothing to lose yet: the wait for a silence starts at the first contact.
    s.heardMs = nowMs;
    return 0;
  }
  const uint32_t quiet = nowMs - s.heardMs;
  if (!s.silent) {
    if (quiet < BUS_SILENCE_AFTER_MS) return 0;
    s.silent = true;
    s.savedMinutes = 0;
    s.reinitOpen = false;
    s.restarted = false;
    s.trafficOpen = false;
    busSilenceBump(record.silences);
    record.lastMinutes = busSilenceMinutes(quiet);
    if (record.longestMinutes < record.lastMinutes) record.longestMinutes = record.lastMinutes;
    record.flags = lineLow ? BUS_RECORD_FLAG_LINE_LOW : 0;
    s.reinitMs = nowMs - BUS_SILENCE_AFTER_MS;  // the first restart is due now
    todo |= BUS_SILENCE_DO_WATCH_TRAFFIC;
  }
  if (traffic && !s.trafficOpen && !(record.flags & BUS_RECORD_FLAG_TRAFFIC)) {
    s.trafficOpen = true;
    s.trafficMs = nowMs;
  }
  if (s.trafficOpen && nowMs - s.trafficMs >= BUS_SILENCE_TRAFFIC_OWN_MS) {
    // Still nothing for this unit: it was traffic for others. Written with
    // the next mark of the length.
    s.trafficOpen = false;
    record.flags |= BUS_RECORD_FLAG_TRAFFIC;
  }
  // A held line is the other heal's; a restart of the hardware under it would
  // only be counted here for nothing.
  if (!lineLow && nowMs - s.reinitMs >= BUS_SILENCE_AFTER_MS) {
    s.reinitMs = nowMs;
    s.reinitOpen = true;
    busSilenceBump(record.reinits);
    todo |= BUS_SILENCE_DO_REINIT;
  }
  const uint8_t minutes = busSilenceMinutes(quiet);
  const uint8_t first = busSilenceMinutes(2 * BUS_SILENCE_AFTER_MS);
  const uint8_t mark = s.savedMinutes == 0 ? first
                       : s.savedMinutes < 0x80 ? (uint8_t)(s.savedMinutes * 2) : 0xFF;
  if (minutes >= mark && minutes > s.savedMinutes) {
    s.savedMinutes = minutes;
    record.lastMinutes = minutes;
    if (minutes > record.longestMinutes) record.longestMinutes = minutes;
    todo |= BUS_SILENCE_DO_SAVE;
  }
  if (restartAllowed && !s.restarted && quiet >= BUS_SILENCE_RESTART_MS) {
    s.restarted = true;
    s.savedMinutes = minutes == 0 ? 1 : minutes;
    record.lastMinutes = minutes;
    if (minutes > record.longestMinutes) record.longestMinutes = minutes;
    record.flags |= BUS_RECORD_FLAG_RESTARTED;
    busSilenceBump(record.selfRestarts);
    todo |= BUS_SILENCE_DO_SAVE | BUS_SILENCE_DO_RESTART;
  }
  return todo;
}
