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
