#pragma once
// Pure TWI self-heal policy (#489). A Nano's TWI peripheral can wedge holding
// SDA low with SCL free: no master clock-out releases it (bench: 20 clocks,
// still I2C_SDA_HELD_LOW), and the 8 s WDT never fires because loop() keeps
// running — the whole row's bus stays dead until a power cycle. loop() samples
// SDA and SCL (either held low kills the bus); a line held past
// TWI_HEAL_HOLD_MS arms a peripheral reset (TWEN off → Wire.begin). Every unit on the bus sees the same low line and resets, which
// is harmless; only the unit whose release freed the line counts it, so the
// count names the culprit. Natively tested by test_twi_heal; the AVR glue is in
// UnitI2CProtocol.ino (bench tier).

#include <stdint.h>

// No legitimate transfer holds a line low this long: a byte at the ESP-01's
// bit-banged ~100 kHz is under 1 ms, and a Nano's ISR stretches SCL for µs.
#define TWI_HEAL_HOLD_MS     50UL
// Minimum gap between resets while the line stays held by another unit.
#define TWI_HEAL_COOLDOWN_MS 1000UL

struct TwiHealState {
  bool     lowSeen = false;
  uint32_t lowSinceMs = 0;
  bool     everReset = false;
  uint32_t lastResetMs = 0;
  uint8_t  selfResets = 0;  // since boot, saturating: resets that freed the bus
};

inline bool twiHealShouldReset(TwiHealState& s, bool lineHeld, uint32_t nowMs) {
  if (!lineHeld) {
    s.lowSeen = false;
    return false;
  }
  if (!s.lowSeen) {
    s.lowSeen = true;
    s.lowSinceMs = nowMs;
  }
  if (nowMs - s.lowSinceMs < TWI_HEAL_HOLD_MS) return false;
  return !s.everReset || nowMs - s.lastResetMs >= TWI_HEAL_COOLDOWN_MS;
}

// releasedByUs: both lines read high right after our TWI let go of the pins.
inline void twiHealNoteReset(TwiHealState& s, uint32_t nowMs,
                             bool releasedByUs) {
  s.everReset = true;
  s.lastResetMs = nowMs;
  if (releasedByUs && s.selfResets < 0xFF) s.selfResets++;
}

// --- Deaf-slave check (#502) ------------------------------------------------
// The held-line heal above cannot see a slave whose TWI registers were
// corrupted while both lines stay free: TWCR without TWEN/TWIE/TWEA, or TWAR
// no longer holding this unit's address plus the general-call enable. Such a
// unit NACKs its own address indefinitely while loop() keeps the watchdog fed.
// loop() samples both registers; a config that stays wrong for TWI_HEAL_HOLD_MS
// arms the same peripheral re-init, fired at an instant when both lines are
// free. TWEA drops legitimately for about a byte time at the end of a slave
// transmit — far inside the hold window. A held line is the other heal's job.

// TWCR bits a listening slave needs: TWEA (bit 6) | TWEN (bit 2) | TWIE
// (bit 0). A literal so this header stays host-buildable; the AVR glue
// static_asserts it against the register definitions.
#define TWI_LISTEN_TWCR_MASK 0x45
// TWAR bit 0 is TWGCE (general-call enable); setup() arms it after Wire.begin.
#define TWI_LISTEN_TWAR_GCE  0x01

inline bool twiListenConfigIntact(uint8_t twcr, uint8_t twar, uint8_t address) {
  return (twcr & TWI_LISTEN_TWCR_MASK) == TWI_LISTEN_TWCR_MASK &&
         twar == (uint8_t)((uint8_t)(address << 1) | TWI_LISTEN_TWAR_GCE);
}

struct TwiDeafState {
  bool     badSeen = false;
  uint32_t badSinceMs = 0;
  bool     everReset = false;
  uint32_t lastResetMs = 0;
  uint8_t  resets = 0;  // since boot, saturating
};

inline bool twiDeafShouldReset(TwiDeafState& s, bool configIntact,
                               bool linesFree, uint32_t nowMs) {
  if (configIntact) {
    s.badSeen = false;
    return false;
  }
  if (!s.badSeen) {
    s.badSeen = true;
    s.badSinceMs = nowMs;
  }
  if (nowMs - s.badSinceMs < TWI_HEAL_HOLD_MS) return false;
  if (!linesFree) return false;
  return !s.everReset || nowMs - s.lastResetMs >= TWI_HEAL_COOLDOWN_MS;
}

inline void twiDeafNoteReset(TwiDeafState& s, uint32_t nowMs) {
  s.everReset = true;
  s.lastResetMs = nowMs;
  s.badSeen = false;
  if (s.resets < 0xFF) s.resets++;
}
