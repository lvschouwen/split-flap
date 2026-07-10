// UnitBus.cpp (#203) — straight port of v1's ServiceFlapFunctions.ino bus
// core onto the S3 (same blocking Wire transactions, same timing constants;
// see UnitBus.h for the ownership rules). v1's volatile staging flags
// (busReprobePending, i2cBusBusy, ...) have no counterpart here: the
// display command queue serializes everything.

#include "UnitBus.h"

#include <Arduino.h>
#include <Wire.h>

#include "HelpersSerialHandling.h"
#include "SplitFlapProtocol.h"
#include "TwibootProtocol.h"
#include "UnitProtocolHelpers.h"

// Unit bus pins + clock (rationale in UnitBus.h; S3 pin budget in
// platformio.ini).
static constexpr int UNIT_BUS_SDA_PIN = 8;
static constexpr int UNIT_BUS_SCL_PIN = 9;
static constexpr uint32_t UNIT_BUS_FREQ_HZ = 100000;

// Delay between an opcode write and the read-back clocking, so the slave's
// receiveEvent ISR has time to flip its pending*Response flag.
static constexpr uint32_t UNIT_RESPONSE_SETTLE_MS = 2;
// How long waitForDisplayToStop() keeps polling before assuming a unit is
// physically stuck (status byte pegged at 1) and moving on.
static constexpr uint32_t SHOW_STUCK_TIMEOUT_MS = 30000;

static int toI2cAddress(int unitIndex) {
  return SFP_I2C_ADDRESS_BASE + unitIndex;
}

void unitBusInit() {
  Wire.begin(UNIT_BUS_SDA_PIN, UNIT_BUS_SCL_PIN, UNIT_BUS_FREQ_HZ);
}

// Shared opcode-write-then-read-back transaction behind every readUnit*
// helper (v1 #154): write the opcode, settle, clock `n` bytes into `buf`.
// Old firmware that predates an opcode silently drops the write (the opcode
// namespace is reserved) but answers reads with its 1-byte rotation status,
// so a short reply means "unsupported" — drain the RX buffer and fail.
// `buf` holds all `n` bytes only on success.
static bool queryUnit(int i2cAddress, uint8_t opcode, uint8_t* buf, uint8_t n) {
  Wire.beginTransmission(i2cAddress);
  Wire.write(opcode);
  if (Wire.endTransmission() != 0) return false;
  delay(UNIT_RESPONSE_SETTLE_MS);
  uint8_t got = Wire.requestFrom((uint8_t)i2cAddress, n);
  if (got != n) {
    while (Wire.available()) Wire.read();
    return false;
  }
  for (uint8_t i = 0; i < n; i++) buf[i] = Wire.read();
  return true;
}

// Reads the 8-byte health/status payload via CMD_GET_STATUS (v1 #47).
// Returns true on success. Short replies (old firmware predating this
// opcode) or Wire failures return false without touching `out`.
static bool readUnitStatus(int i2cAddress, UnitStatus& out) {
  uint8_t buf[8];
  if (!queryUnit(i2cAddress, (uint8_t)SFP_CMD_GET_STATUS, buf, 8)) return false;
  out.flags                 = buf[0];
  out.mcusrAtBoot           = buf[1];
  out.lifetimeBrownoutCount = buf[2];
  out.lifetimeWatchdogCount = buf[3];
  out.uptimeSeconds         = ((uint16_t)buf[4] << 8) | (uint16_t)buf[5];
  out.badCommandCount       = buf[6];
  // Byte 7 is last-homing-step / 16 (saturating); decode by reversing.
  out.lastHomingStepCount   = (uint16_t)buf[7] << 4;
  return true;
}

// Asks a sketch-mode unit for its firmware GIT_REV via CMD_GET_VERSION.
// Writes up to 8 printable ASCII bytes into `out` (null-terminated).
// Rejects `"` and `\` at the I2C boundary (v1 #140): the string is emitted
// raw into the health JSON, and a glitched read carrying either would break
// JSON.parse — a real git short-rev never contains them.
static bool readUnitVersion(int i2cAddress, char* out) {
  out[0] = '\0';
  uint8_t buf[8];
  if (!queryUnit(i2cAddress, (uint8_t)SFP_CMD_GET_VERSION, buf, 8)) return false;
  uint8_t len = 0;
  for (; len < 8; len++) {
    if (buf[len] == 0) break;
    if (buf[len] < 32 || buf[len] > 126) return false;
    if (buf[len] == '"' || buf[len] == '\\') return false;
  }
  if (len == 0) return false;
  for (uint8_t i = 0; i < len; i++) out[i] = (char)buf[i];
  out[len] = '\0';
  return true;
}

// Reads back the unit's currently displayed letter index via CMD_GET_LETTER
// (v1 #106). New firmware replies 2 bytes: index + bitwise complement; old
// firmware replies with the 1-byte status fallback, so a short read (or a
// failed complement check) returns false and `out` is untouched.
static bool readUnitDisplayedLetter(int i2cAddress, int& out) {
  uint8_t buf[2];
  if (!queryUnit(i2cAddress, (uint8_t)SFP_CMD_GET_LETTER, buf, 2)) return false;
  if (!letterReadbackValid(buf[0], buf[1], FLAP_AMOUNT)) return false;
  out = buf[0];
  return true;
}

// Writes twiboot's CMD_ACCESS_MEMORY + CHIPINFO request, reads the 8-byte
// chipinfo response and checks whether the signature matches the ATmega328P.
// Safe to call against a sketch-running unit: the patched Unit.ino ignores
// writes of length != 2, so probing doesn't rotate the drum as a side effect.
static bool isUnitInBootloader(int i2cAddress) {
  Wire.beginTransmission(i2cAddress);
  Wire.write((uint8_t)TWIBOOT_CMD_ACCESS_MEMORY);
  Wire.write((uint8_t)TWIBOOT_MEMTYPE_CHIPINFO);
  Wire.write((uint8_t)0x00);
  Wire.write((uint8_t)0x00);
  if (Wire.endTransmission(false) != 0) return false;
  uint8_t got = Wire.requestFrom((uint8_t)i2cAddress, (uint8_t)8);
  if (got < 3) {
    while (Wire.available()) Wire.read();
    return false;
  }
  uint8_t sig0 = Wire.read();
  uint8_t sig1 = Wire.read();
  uint8_t sig2 = Wire.read();
  while (Wire.available()) Wire.read();
  return isAtmega328pSignature(sig0, sig1, sig2);
}

// Write letter index and wire speed to a single unit. Returns
// Wire.endTransmission() status (0 = success) so callers can tally
// bus-level failures (v1 #121).
static int writeToUnit(int unitIndex, uint8_t letter, uint8_t unitSpeed) {
  Wire.beginTransmission(toI2cAddress(unitIndex));
  Wire.write(letter);
  Wire.write(unitSpeed);
  return Wire.endTransmission();
}

// Checks if a single unit is moving (1-byte rotation status). Called ~10x/s
// from isDisplayMoving() — must be quiet on /log when nothing is wrong.
static int checkIfMoving(int unitIndex) {
  int i2cAddress = toI2cAddress(unitIndex);
  Wire.requestFrom((uint8_t)i2cAddress, (uint8_t)1);
  int active = Wire.available() ? Wire.read() : -1;
  if (active == -1) {
    // Wake-up ping: empty transmission pulses the TWI peripheral.
    Wire.beginTransmission(i2cAddress);
    Wire.endTransmission();
  }
  return active;
}

// True while any sketch-mode unit reports rotation. A silent read (-1) from
// such a unit is treated as "idle" rather than "sleeping" so the master
// never deadlocks on a transiently unresponsive (or physically absent) unit.
static bool isDisplayMoving(const UnitFacts* facts, int width) {
  for (int unitIndex = 0; unitIndex < width; unitIndex++) {
    if (facts[unitIndex].state != 1) continue;
    if (checkIfMoving(unitIndex) == 1) return true;
  }
  return false;
}

// Waits until no unit reports rotation, with the SHOW_STUCK_TIMEOUT_MS
// stuck-unit cap. The delay(100) yields displayTask's core between polls.
// (v1's /stop abort has no v2 counterpart yet — the queue model needs a
// different cancellation design; deliberately out of slice A.)
static void waitForDisplayToStop(const UnitFacts* facts, int width) {
  uint32_t waitStart = millis();
  uint32_t lastWaitLog = 0;
  while (isDisplayMoving(facts, width)) {
    if (millis() - waitStart > SHOW_STUCK_TIMEOUT_MS) {
      SerialPrintln(F("Display-stop wait timed out — assuming a unit is stuck, continuing anyway"));
      break;
    }
    if (millis() - lastWaitLog > 5000) {
      SerialPrintln(F("Waiting for display to stop"));
      lastWaitLog = millis();
    }
    delay(100);
  }
}

// Closed-loop letter verification (v1 #106). Reads back each written unit's
// displayed letter and re-sends once on mismatch — a corrupted or dropped
// I2C write no longer leaves the wrong character standing until the next
// message. Units on pre-#106 firmware fail the readback and are skipped.
static void verifyAndResendLetters(const UnitFacts* facts, int width,
                                   const uint8_t* letters, uint8_t unitSpeed) {
  int resent = 0;
  for (int unitIndex = 0; unitIndex < width; unitIndex++) {
    if (facts[unitIndex].state != 1) continue;
    int shown;
    if (!readUnitDisplayedLetter(toI2cAddress(unitIndex), shown)) continue;
    if (shown == letters[unitIndex]) continue;
    SerialPrintf("Unit %d shows the wrong letter index — re-sending\n",
                 unitIndex);
    writeToUnit(unitIndex, letters[unitIndex], unitSpeed);
    resent++;
  }
  if (resent > 0) {
    SerialPrintf("Letter verification re-sent %d unit(s)\n", resent);
    waitForDisplayToStop(facts, width);
  }
}

void unitBusProbe(UnitFacts* facts, int maxUnits) {
  SerialPrintln(F("Scanning I2C bus for units..."));
  int detected = 0;
  for (int unitIndex = 0; unitIndex < maxUnits; unitIndex++) {
    facts[unitIndex] = UnitFacts{};  // silent, fw unknown, status invalid
    int i2cAddress = toI2cAddress(unitIndex);
    Wire.beginTransmission(i2cAddress);
    if (Wire.endTransmission() != 0) continue;

    bool inBootloader = isUnitInBootloader(i2cAddress);
    facts[unitIndex].state = inBootloader ? 2 : 1;
    detected++;

    SerialPrintf("- unit at 0x%02x", i2cAddress);
    if (inBootloader) {
      SerialPrintln(F(" is in BOOTLOADER mode"));
      continue;
    }
    // fwStatus stays 2 (unknown) even on a good read: slice A bundles no
    // unit hex, so there is no rev to be outdated against — slice C's
    // reflash brings the comparison target (v1 compared BUNDLED_UNIT_REV).
    if (readUnitVersion(i2cAddress, facts[unitIndex].version)) {
      SerialPrintf(" is running sketch (fw %s)\n", facts[unitIndex].version);
    } else {
      SerialPrintln(F(" is running sketch (fw UNKNOWN — likely predates version opcode)"));
    }
  }
  SerialPrintf("I2C scan complete. Detected %d", detected);
  SerialPrintf("/%d possible units.\n", maxUnits);
}

void unitBusPollHealth(UnitFacts* facts, int maxUnits) {
  for (int i = 0; i < maxUnits; i++) {
    facts[i].statusValid = false;
    // Only sketch-running units (state 1) answer CMD_GET_STATUS; a unit in
    // bootloader (2) or silent (0) is left invalid so it renders as a gap
    // in the table and never counts toward the faulty total.
    if (facts[i].state != 1) continue;
    UnitStatus s;
    if (readUnitStatus(toI2cAddress(i), s)) {
      facts[i].status = s;
      facts[i].statusValid = true;
    }
  }
}

int unitBusShowFrame(const UnitFacts* facts, int width,
                     const uint8_t* letters, int unitSpeed) {
  // Entry wait: never interleave a new frame into a still-rotating display.
  waitForDisplayToStop(facts, width);

  int writeErrors = 0;
  for (int unitIndex = 0; unitIndex < width; unitIndex++) {
    // Skip slots the probe did not find a sketch-running unit on: writing
    // to absent addresses stalls isDisplayMoving() and a dead unit
    // mid-display must not wedge the whole frame (v1 behavior).
    if (facts[unitIndex].state != 1) continue;
    if (writeToUnit(unitIndex, letters[unitIndex], (uint8_t)unitSpeed) != 0) {
      writeErrors++;
    }
  }

  waitForDisplayToStop(facts, width);
  verifyAndResendLetters(facts, width, letters, (uint8_t)unitSpeed);
  return writeErrors;
}
