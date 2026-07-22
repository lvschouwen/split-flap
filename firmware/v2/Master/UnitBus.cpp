// UnitBus.cpp (#203) — straight port of v1's ServiceFlapFunctions.ino bus
// core onto the S3 (same blocking Wire transactions, same timing constants;
// see UnitBus.h for the ownership rules). v1's volatile staging flags
// (busReprobePending, i2cBusBusy, ...) have no counterpart here: the
// display command queue serializes everything.

#include "UnitBus.h"

#include <Arduino.h>
#include <Wire.h>

#include <atomic>

#include "BuildVersion.h"  // BUNDLED_UNIT_REV (#205)
#include "HelpersSerialHandling.h"
#include "MaintenancePolicy.h"
#include "RenderStagger.h"  // sub-frame inrush stagger (#324)
#include "SplitFlapProtocol.h"
#include "TaskWatchdog.h"  // wdtFeed() (#314)
#include "TwibootProtocol.h"
#include "UnitProtocolHelpers.h"

// Stop-abort signal (#204) — see UnitBus.h for the contract.
static std::atomic<bool> abortRequested{false};

void unitBusRequestAbort() { abortRequested.store(true); }
void unitBusClearAbort() { abortRequested.store(false); }
bool unitBusAbortRequested() { return abortRequested.load(); }

// Unit bus pins + clock (rationale in UnitBus.h; S3 pin budget in
// platformio.ini).
static constexpr int UNIT_BUS_SDA_PIN = 8;
static constexpr int UNIT_BUS_SCL_PIN = 9;
static constexpr uint32_t UNIT_BUS_FREQ_HZ = 100000;

// Delay between an opcode write and the read-back clocking, so the slave's
// receiveEvent ISR has time to flip its pending*Response flag.
static constexpr uint32_t UNIT_RESPONSE_SETTLE_MS = 2;
// How long waitForDisplayToStop() keeps polling before assuming a unit is
// physically stuck (status byte pegged at 1) and moving on. This and any
// other display stuck-timeout must stay <= the TWDT timeout, OR the poll
// loop must feed the watchdog itself (it does, #314) — a jammed flap is a
// mechanical condition this code deliberately survives, not a reboot cause.
static constexpr uint32_t SHOW_STUCK_TIMEOUT_MS = 30000;

static int toI2cAddress(int unitIndex) {
  return SFP_I2C_ADDRESS_BASE + unitIndex;
}

void unitBusInit() {
  if (!Wire.begin(UNIT_BUS_SDA_PIN, UNIT_BUS_SCL_PIN, UNIT_BUS_FREQ_HZ)) {
    SerialPrintln(F("I2C unit bus init failed"));
  }
}

// IDF 5.5's esp_driver_i2c only clears its bus-level "transaction contains a
// read" flag when the read's RX path completes — a failed requestFrom
// (address NACK from a browned-out unit, timeout on a broken bus) leaves it
// set, and the next zero-length probe then runs the ISR receive handler
// against a transaction with no read op: the RX FIFO is copied through a
// NULL data pointer and the master panics (StoreProhibited, #207). Tearing
// the bus down and rebuilding it destroys the stale driver state, so every
// failed read must pass through here before the next probe touches the bus.
static void recoverBusAfterFailedRead() {
  Wire.end();
  unitBusInit();
}

// Bus transaction counters for the System tab (#245). displayTask is the
// only writer (sole Wire toucher); netTask's stats sampler reads them.
// Scope: sketch-protocol traffic only — frames, queries and maintenance
// ops. Deliberately NOT counted: the ~10 Hz checkIfMoving() idle polls
// (keep-alive noise would drown the signal) and the twiboot reflash
// page stream (#205 reports its own progress/failures). err counts
// write AND read-back failures while tx counts write transactions only,
// so err can legitimately exceed tx under read-heavy failure.
static std::atomic<uint32_t> busTxCount{0};
static std::atomic<uint32_t> busErrCount{0};

uint32_t unitBusTxCount() { return busTxCount.load(); }
uint32_t unitBusErrCount() { return busErrCount.load(); }

static int countedTransmission() {
  int status = Wire.endTransmission();
  busTxCount.fetch_add(1);
  if (status != 0) busErrCount.fetch_add(1);
  return status;
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
  if (countedTransmission() != 0) return false;
  delay(UNIT_RESPONSE_SETTLE_MS);
  uint8_t got = Wire.requestFrom((uint8_t)i2cAddress, n);
  if (got != n) {
    while (Wire.available()) Wire.read();
    busErrCount.fetch_add(1);  // short/failed read leg (#245)
    recoverBusAfterFailedRead();
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

// Reads the unit's revolution odometer via CMD_GET_ODOMETER (#231): 5 bytes,
// uint32 LE + masked XOR checksum. Old firmware answers the unknown opcode
// with its 1-byte status fallback + bus padding — odometerReadbackValid
// rejects that instead of "verifying" garbage as a count.
static bool readUnitOdometer(int i2cAddress, uint32_t& out) {
  uint8_t buf[5];
  if (!queryUnit(i2cAddress, (uint8_t)SFP_CMD_GET_ODOMETER, buf, 5)) return false;
  return odometerReadbackValid(buf, out);
}

// Reads the unit's drift diagnostics via CMD_GET_DIAG (#263/#264): 6 bytes,
// masked XOR checksum + letter range check. Old firmware answers the
// unknown opcode with its 1-byte status fallback — diagReadbackValid
// rejects that (#106 class).
static bool readUnitDiag(int i2cAddress, UnitDiagReading& out) {
  uint8_t buf[6];
  if (!queryUnit(i2cAddress, (uint8_t)SFP_CMD_GET_DIAG, buf, 6)) return false;
  return diagReadbackValid(buf, (uint8_t)FLAP_AMOUNT, out);
}

// Folds a diag read into the slot's facts; clears diagValid first so a unit
// that stops answering (or was reflashed to pre-diag firmware) never keeps
// serving stale drift numbers.
static void refreshUnitDiag(UnitFacts& fact, int i2cAddress) {
  fact.diagValid = false;
  UnitDiagReading d;
  if (!readUnitDiag(i2cAddress, d)) return;
  fact.physLetter = d.physicalLetter;
  fact.driftFlags = d.flags;
  fact.driftEvents = d.driftEvents;
  fact.lastDriftSteps = d.lastDriftSteps;
  fact.diagValid = true;
}

// Reads the unit's supply-Vcc / free-RAM / commanded-position diagnostics via
// CMD_GET_VITALS (#306): 8 bytes, masked XOR checksum. Pre-vitals firmware
// answers the unknown opcode with its 1-byte status fallback + bus padding —
// vitalsReadbackValid rejects that instead of "verifying" garbage (#106 class).
static bool readUnitVitals(int i2cAddress, UnitVitals& out) {
  uint8_t buf[VITALS_REPLY_LEN];
  if (!queryUnit(i2cAddress, (uint8_t)SFP_CMD_GET_VITALS, buf, VITALS_REPLY_LEN)) return false;
  return vitalsReadbackValid(buf, out);
}

// Folds a vitals read into the slot; clears vitalsValid first so a unit that
// stops answering (or was reflashed to pre-vitals firmware) never keeps
// serving a stale Vcc reading (same discipline as refreshUnitDiag).
static void refreshUnitVitals(UnitFacts& fact, int i2cAddress) {
  fact.vitalsValid = false;
  UnitVitals v;
  if (!readUnitVitals(i2cAddress, v)) return;
  fact.vitals = v;
  fact.vitalsValid = true;
}

// Reads the unit's current calOffset (int16 LE) via CMD_GET_OFFSET. Returns
// true on success; `out` is untouched on failure (old firmware predating
// v1 #32 answers short — the drain-and-fail path).
static bool readUnitOffset(int i2cAddress, int16_t& out) {
  uint8_t buf[2];
  if (!queryUnit(i2cAddress, (uint8_t)SFP_CMD_GET_OFFSET, buf, 2)) return false;
  out = (int16_t)((uint16_t)buf[0] | ((uint16_t)buf[1] << 8));
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
    recoverBusAfterFailedRead();
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
  return countedTransmission();
}

// Checks if a single unit is moving (1-byte rotation status). Called ~10x/s
// from isDisplayMoving() — must be quiet on /log when nothing is wrong.
static int checkIfMoving(int unitIndex) {
  int i2cAddress = toI2cAddress(unitIndex);
  Wire.requestFrom((uint8_t)i2cAddress, (uint8_t)1);
  int active = Wire.available() ? Wire.read() : -1;
  if (active == -1) {
    // The failed read must not leave stale driver state behind (#207): the
    // wake-up ping below is exactly the probe-after-failed-read shape that
    // panics the master. Recover first, then ping.
    recoverBusAfterFailedRead();
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
// The abort signal (#204) short-circuits the wait so a queued Stop takes
// effect promptly instead of sitting out a stuck-unit timeout.
static void waitForDisplayToStop(const UnitFacts* facts, int width) {
  uint32_t waitStart = millis();
  uint32_t lastWaitLog = 0;
  while (isDisplayMoving(facts, width)) {
    wdtFeed();  // #314: feed the TWDT through a legitimate stuck-flap wait
    if (abortRequested.load()) {
      SerialPrintln(F("Display-stop wait aborted by /stop"));
      break;
    }
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

// Canary probe (#213): no unit can exist at these addresses — they sit in
// I2C's reserved range, far outside the DIP window (0x01..0x10, twiboot
// included). A healthy bus NACKs both; a floating/held-low bus ACKs every
// address (the S3 controller samples the dead line as ACK and valid-looking
// 0x00 data, and IDF's i2c_master_probe maps unmapped outcomes to OK — this
// beat #209's status-read gate on the bench). Both must ACK to declare the
// bus lying, so a single glitched probe on a real display can't blank the
// whole scan. This restores what v1's bit-banged master did for free: its
// write_start() refused to talk on a line that wasn't idle-high.
static bool busReadsAsFloating() {
  static constexpr uint8_t CANARY_ADDRESSES[] = {0x3B, 0x7B};
  int acks = 0;
  for (uint8_t address : CANARY_ADDRESSES) {
    Wire.beginTransmission(address);
    if (Wire.endTransmission() == 0) acks++;
  }
  if (acks == 1) {
    SerialPrintln(F("I2C canary: one reserved address ACKed — bus suspect, "
                    "continuing scan"));
  }
  return acks == 2;
}

void unitBusProbe(UnitFacts* facts, int maxUnits) {
  SerialPrintln(F("Scanning I2C bus for units..."));
  if (busReadsAsFloating()) {
    for (int unitIndex = 0; unitIndex < maxUnits; unitIndex++) {
      facts[unitIndex] = UnitFacts{};  // silent
    }
    SerialPrintln(F("I2C bus reads as floating (phantom ACKs on reserved "
                    "addresses) — reporting 0 units"));
    return;
  }
  int detected = 0;
  for (int unitIndex = 0; unitIndex < maxUnits; unitIndex++) {
    facts[unitIndex] = UnitFacts{};  // silent, fw unknown, status invalid
    int i2cAddress = toI2cAddress(unitIndex);
    Wire.beginTransmission(i2cAddress);
    if (Wire.endTransmission() != 0) continue;

    bool inBootloader = isUnitInBootloader(i2cAddress);
    if (!inBootloader) {
      // A probe ACK alone is not proof of life: IDF's i2c_master_probe
      // returns OK for outcomes it doesn't map, so a floating bus scans as
      // a full row of phantom units (#209). Every unit firmware generation
      // answers the 1-byte rotation-status read, so require it before
      // trusting the slot (checkIfMoving recovers the bus itself when a
      // phantom NACKs the read, #207). Bootloader units are exempt: they
      // are only classified via a successful chipinfo read above.
      if (checkIfMoving(unitIndex) < 0) continue;
    }
    facts[unitIndex].state = inBootloader ? 2 : 1;
    detected++;

    SerialPrintf("- unit at 0x%02x", i2cAddress);
    if (inBootloader) {
      SerialPrintln(F(" is in BOOTLOADER mode"));
      continue;
    }
    if (readUnitVersion(i2cAddress, facts[unitIndex].version)) {
      // The build bundles a unit hex (#205), so a readable rev grades for
      // real against BUNDLED_UNIT_REV — 0 ok / 1 outdated.
      facts[unitIndex].fwStatus =
          unitFwStatusFromRev(facts[unitIndex].version, BUNDLED_UNIT_REV);
      SerialPrintf(" is running sketch (fw %s%s)\n",
                   facts[unitIndex].version,
                   facts[unitIndex].fwStatus == 0 ? "" : " — OUTDATED");
    } else {
      SerialPrintln(F(" is running sketch (fw UNKNOWN — likely predates version opcode)"));
    }
    // Offset is a probe-time fact (#204): GET /unit/offset serves from the
    // snapshot, so every sketch unit's stored offset is read here. Old
    // firmware (pre-#32) fails the read and stays offsetValid=false.
    int16_t offset;
    if (readUnitOffset(i2cAddress, offset)) {
      facts[unitIndex].offset = offset;
      facts[unitIndex].offsetValid = true;
    }
    // Odometer is a probe-time fact like the offset (#231); pre-odometer
    // firmware fails the checksum and stays odometerValid=false.
    uint32_t odometer;
    if (readUnitOdometer(i2cAddress, odometer)) {
      facts[unitIndex].odometer = odometer;
      facts[unitIndex].odometerValid = true;
    }
    // Drift diagnostics ride the probe too (#263/#264); pre-diag firmware
    // fails the checksum and stays diagValid=false.
    refreshUnitDiag(facts[unitIndex], i2cAddress);
    // Supply-Vcc diagnostics ride the probe too (#306); pre-vitals firmware
    // fails the checksum and stays vitalsValid=false.
    refreshUnitVitals(facts[unitIndex], i2cAddress);
  }
  SerialPrintf("I2C scan complete. Detected %d", detected);
  SerialPrintf("/%d possible units.\n", maxUnits);
}

bool unitBusPollHealthOne(UnitFacts* facts, int i) {
  facts[i].statusValid = false;
  // Only sketch-running units (state 1) answer CMD_GET_STATUS; a unit in
  // bootloader (2) or silent (0) is left invalid so it renders as a gap
  // in the table and never counts toward the faulty total.
  if (facts[i].state != 1) return false;
  UnitStatus s;
  bool ok = readUnitStatus(toI2cAddress(i), s);
  if (ok) {
    facts[i].status = s;
    facts[i].statusValid = true;
  }
  // Refresh the odometer with the same validity semantics as the status:
  // reset, then re-read, so a unit that stops answering (or was reflashed
  // to pre-odometer firmware) doesn't keep serving a stale count (#231).
  uint32_t odometer;
  if (readUnitOdometer(toI2cAddress(i), odometer)) {
    facts[i].odometer = odometer;
    facts[i].odometerValid = true;
  }
  // Drift diagnostics refresh on the same cadence (#263/#264).
  refreshUnitDiag(facts[i], toI2cAddress(i));
  // Supply-Vcc diagnostics refresh on the same cadence (#306).
  refreshUnitVitals(facts[i], toI2cAddress(i));
  // ok == the CMD_GET_STATUS read succeeded — the heartbeat liveness signal
  // (#310); the caller folds it into the miss counter.
  return ok;
}

void unitBusPollHealth(UnitFacts* facts, int maxUnits) {
  for (int i = 0; i < maxUnits; i++) {
    unitBusPollHealthOne(facts, i);
  }
}

int unitBusShowFrame(const UnitFacts* facts, int width,
                     const uint8_t* letters, int unitSpeed) {
  // Entry wait: never interleave a new frame into a still-rotating display.
  waitForDisplayToStop(facts, width);

  int writeErrors = 0;
  int commanded = 0;
  for (int unitIndex = 0; unitIndex < width; unitIndex++) {
    // Skip slots the probe did not find a sketch-running unit on: writing
    // to absent addresses stalls isDisplayMoving() and a dead unit
    // mid-display must not wedge the whole frame (v1 behavior).
    if (facts[unitIndex].state != 1) continue;
    // #324: spread the flap inrush — pause before opening each new group so a
    // full row's steppers don't spin up at once and brown out the rail.
    if (renderStaggerShouldSettle(commanded, RENDER_STAGGER_BATCH)) {
      delay(RENDER_STAGGER_SETTLE_MS);
    }
    if (writeToUnit(unitIndex, letters[unitIndex], (uint8_t)unitSpeed) != 0) {
      writeErrors++;
    }
    commanded++;
  }

  waitForDisplayToStop(facts, width);
  verifyAndResendLetters(facts, width, letters, (uint8_t)unitSpeed);
  return writeErrors;
}

// --- calibration + provisioning (#204) — straight v1 ports --------------------
// Payload encodings live in MaintenancePolicy.h so the negative int16/int8
// wire bytes are asserted natively.

int unitBusWriteOffset(int i2cAddress, int16_t value) {
  uint8_t payload[2];
  maintEncodeOffsetLE(value, payload);
  Wire.beginTransmission(i2cAddress);
  Wire.write((uint8_t)SFP_CMD_SET_OFFSET);
  Wire.write(payload[0]);
  Wire.write(payload[1]);
  return countedTransmission();
}

int unitBusJog(int i2cAddress, int steps) {
  Wire.beginTransmission(i2cAddress);
  Wire.write((uint8_t)SFP_CMD_JOG);
  Wire.write(maintEncodeJogByte(steps));
  return countedTransmission();
}

int unitBusHome(int i2cAddress) {
  Wire.beginTransmission(i2cAddress);
  Wire.write((uint8_t)SFP_CMD_HOME);
  return countedTransmission();
}

int unitBusIdentify(int i2cAddress) {
  Wire.beginTransmission(i2cAddress);
  Wire.write((uint8_t)SFP_CMD_IDENTIFY);
  return countedTransmission();
}

int unitBusResetOdometer(int i2cAddress) {
  Wire.beginTransmission(i2cAddress);
  Wire.write((uint8_t)SFP_CMD_RESET_ODOMETER);
  return countedTransmission();
}

int unitBusStartSelfTest(int i2cAddress) {
  Wire.beginTransmission(i2cAddress);
  Wire.write((uint8_t)SFP_CMD_START_SELF_TEST);
  return countedTransmission();
}

bool unitBusReadSelfTest(int i2cAddress, UnitSelfTestReading& out) {
  uint8_t buf[9];
  if (!queryUnit(i2cAddress, (uint8_t)SFP_CMD_GET_SELF_TEST, buf, 9)) {
    return false;
  }
  return selfTestReadbackValid(buf, out);
}

// The unit watchdog-resets into twiboot, which listens ~1 s on the
// DIP-derived address. HARD RULE (v1 #88): never probe while a unit can be
// in its twiboot window — the CHIPINFO query pins the bootloader alive.
int unitBusRebootToBootloader(int i2cAddress) {
  Wire.beginTransmission(i2cAddress);
  Wire.write((uint8_t)SFP_CMD_ENTER_BOOTLOADER);
  return countedTransmission();
}

int unitBusSetAddress(int i2cAddress, uint8_t newAddress) {
  Wire.beginTransmission(i2cAddress);
  Wire.write((uint8_t)SFP_CMD_SET_I2C_ADDRESS);
  Wire.write(newAddress);
  return countedTransmission();
}

int unitBusClearAddress(int i2cAddress) {
  Wire.beginTransmission(i2cAddress);
  Wire.write((uint8_t)SFP_CMD_CLEAR_I2C_ADDRESS);
  return countedTransmission();
}

int unitBusBroadcastHome() {
  Wire.beginTransmission((uint8_t)SFP_I2C_GENERAL_CALL_ADDRESS);
  Wire.write((uint8_t)SFP_CMD_HOME);
  return countedTransmission();
}

// --- unit reflash over twiboot (#205) — v1 ServiceFirmwareFunctions.ino port --
// Our DIP-patched twiboot listens on the unit's own address (not stock 0x29),
// so every twiboot command targets the unit's address directly. Write NACKs
// during the ~4.5 ms page-program window are EXPECTED (clock stretching is
// disabled in our twiboot build) and are not #207 bus damage — only failed
// READS need recoverBusAfterFailedRead().

static int twibootPing(int addr) {
  Wire.beginTransmission((uint8_t)addr);
  Wire.write((uint8_t)TWIBOOT_CMD_WAIT);
  return Wire.endTransmission();
}

static int twibootExit(int addr) {
  Wire.beginTransmission((uint8_t)addr);
  Wire.write((uint8_t)TWIBOOT_CMD_SWITCH_APPLICATION);
  Wire.write((uint8_t)TWIBOOT_BOOTTYPE_APPLICATION);
  return Wire.endTransmission();
}

// Spin-poll twiboot with CMD_WAIT until it ACKs again (its async flash
// write finished) or the timeout elapses.
static bool twibootWaitReady(int addr, uint16_t timeoutMs) {
  uint32_t deadline = millis() + timeoutMs;
  while ((int32_t)(millis() - deadline) < 0) {
    if (twibootPing(addr) == 0) return true;
    delay(1);
  }
  return false;
}

// Queries twiboot chipinfo and checks it matches the ATmega328P with the
// expected page size.
static bool twibootVerifyChip(int addr) {
  Wire.beginTransmission((uint8_t)addr);
  Wire.write((uint8_t)TWIBOOT_CMD_ACCESS_MEMORY);
  Wire.write((uint8_t)TWIBOOT_MEMTYPE_CHIPINFO);
  Wire.write((uint8_t)0x00);
  Wire.write((uint8_t)0x00);
  if (Wire.endTransmission(false) != 0) return false;
  uint8_t got = Wire.requestFrom((uint8_t)addr, (uint8_t)8);
  if (got != 8) {
    while (Wire.available()) Wire.read();
    recoverBusAfterFailedRead();
    return false;
  }
  uint8_t sig0 = Wire.read(), sig1 = Wire.read(), sig2 = Wire.read();
  uint8_t pageSize = Wire.read();
  while (Wire.available()) Wire.read();  // flash + eeprom sizes, unused
  return isAtmega328pSignature(sig0, sig1, sig2) &&
         pageSize == TWIBOOT_PAGE_SIZE;
}

// Reads one 128-byte page back for post-write verification (v1 #110): same
// CMD_ACCESS_MEMORY framing as the write but with no payload, then a
// repeated-start read — the exact flow twiboot's own host tool uses.
static bool twibootReadFlashPage(int addr, uint16_t flashAddr, uint8_t* out) {
  Wire.beginTransmission((uint8_t)addr);
  Wire.write((uint8_t)TWIBOOT_CMD_ACCESS_MEMORY);
  Wire.write((uint8_t)TWIBOOT_MEMTYPE_FLASH);
  Wire.write((uint8_t)((flashAddr >> 8) & 0xFF));
  Wire.write((uint8_t)(flashAddr & 0xFF));
  if (Wire.endTransmission(false) != 0) return false;
  uint8_t got = Wire.requestFrom((uint8_t)addr, (uint8_t)TWIBOOT_PAGE_SIZE);
  if (got != TWIBOOT_PAGE_SIZE) {
    while (Wire.available()) Wire.read();
    recoverBusAfterFailedRead();
    return false;
  }
  for (int i = 0; i < TWIBOOT_PAGE_SIZE; i++) out[i] = Wire.read();
  return true;
}

// The 132-byte page burst must fit the Wire TX buffer in one transaction —
// a smaller buffer makes Wire.write() silently drop the page tail and every
// verify fails (#243). The -DI2C_BUFFER_LENGTH=256 build flag provides it.
static_assert(I2C_BUFFER_LENGTH >= TWIBOOT_PAGE_SIZE + 4,
              "Wire buffer too small for a twiboot page write — "
              "build with -DI2C_BUFFER_LENGTH=256 (#243)");

static int twibootWriteFlashPage(int addr, uint16_t flashAddr,
                                 const uint8_t* page) {
  Wire.beginTransmission((uint8_t)addr);
  size_t queued = 0;
  queued += Wire.write((uint8_t)TWIBOOT_CMD_ACCESS_MEMORY);
  queued += Wire.write((uint8_t)TWIBOOT_MEMTYPE_FLASH);
  queued += Wire.write((uint8_t)((flashAddr >> 8) & 0xFF));
  queued += Wire.write((uint8_t)(flashAddr & 0xFF));
  for (int i = 0; i < TWIBOOT_PAGE_SIZE; i++) queued += Wire.write(page[i]);
  if (queued != TWIBOOT_PAGE_SIZE + 4) {
    // write() only queues into RAM, so bailing here keeps the truncated
    // page off the bus. With the 256-byte buffer this can only trip when
    // Wire's buffer alloc failed at begin() — a state the core's own
    // endTransmission() early-returns on (mutex left held) too; failing
    // the flash loudly is the least-bad option.
    SerialPrintf("twiboot page burst truncated (%u/%u queued) — "
                 "Wire buffer too small\n",
                 (unsigned)queued, (unsigned)(TWIBOOT_PAGE_SIZE + 4));
    return -1;
  }
  return countedTransmission();
}

// Writes one page and reads it back to verify, with one rewrite attempt on
// mismatch (v1 #110). On persistent failure the caller aborts while the
// unit still sits in twiboot — boot auto-install retries later instead of
// the unit booting a corrupted sketch.
static bool flashAndVerifyPage(int addr, uint16_t flashAddr,
                               const uint8_t* page) {
  for (int attempt = 0; attempt < 2; attempt++) {
    // Ready-wait before the 132-byte burst, then again for the ~4.5 ms SPM
    // cycle. 100/50 ms are the generous v1 values.
    if (!twibootWaitReady(addr, 100)) return false;
    if (twibootWriteFlashPage(addr, flashAddr, page) != 0) return false;
    if (!twibootWaitReady(addr, 50)) return false;

    uint8_t readBuf[TWIBOOT_PAGE_SIZE];
    if (!twibootReadFlashPage(addr, flashAddr, readBuf)) return false;
    if (memcmp(readBuf, page, TWIBOOT_PAGE_SIZE) == 0) return true;
    SerialPrintf("Verify mismatch at page 0x%04x%s\n", flashAddr,
                 attempt == 0 ? " — rewriting once" : " — giving up");
  }
  return false;
}

const char* unitFlashResultName(UnitFlashResult r) {
  switch (r) {
    case UnitFlashResult::Ok:               return "ok";
    case UnitFlashResult::BootloaderSilent: return "bootloader-silent";
    case UnitFlashResult::ChipMismatch:     return "chip-mismatch";
    case UnitFlashResult::PageFailed:       return "page-failed";
    case UnitFlashResult::ExitFailed:       return "exit-failed";
    case UnitFlashResult::PostBootSilent:   return "post-boot-silent";
    default:                                return "aborted";
  }
}

void unitBusWaitBatchIdle(const uint8_t* addrs, int count,
                          uint32_t timeoutMs) {
  if (count <= 0) return;
  SerialPrintf("  waiting for %d unit(s) to come online + finish homing...\n",
               count);
  delay(1000);  // let CMD_REBOOT take effect before polling
  uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    wdtFeed();  // #314: feed the TWDT through the batch-settle poll
    bool allIdle = true;
    for (int k = 0; k < count; k++) {
      // checkIfMoving takes a unit INDEX; addrs carry bus addresses.
      if (checkIfMoving(addrs[k] - SFP_I2C_ADDRESS_BASE) != 0) {
        allIdle = false;
        break;
      }
    }
    if (allIdle) {
      SerialPrintln(F("  batch online + idle"));
      return;
    }
    delay(100);
  }
  SerialPrintln(F("  batch settle timed out — continuing anyway"));
}

UnitFlashResult unitBusFlashUnit(int i2cAddress, const uint8_t* image,
                                 size_t len) {
  SerialPrintf("Flashing unit at 0x%02x (%u bytes)\n", i2cAddress,
               (unsigned)len);

  bool bootloaderLive = false;
  for (int attempt = 0; attempt < 5; attempt++) {
    if (twibootPing(i2cAddress) == 0) { bootloaderLive = true; break; }
    delay(100);
  }
  if (!bootloaderLive) return UnitFlashResult::BootloaderSilent;
  if (!twibootVerifyChip(i2cAddress)) return UnitFlashResult::ChipMismatch;

  size_t pageCount = len / TWIBOOT_PAGE_SIZE;
  for (size_t pageIndex = 0; pageIndex < pageCount; pageIndex++) {
    // #348: ~87 pages × up to ~300 ms each on a degraded bus approaches the
    // 30 s TWDT with no feed — feed per page, not per unit.
    wdtFeed();
    if (abortRequested.load()) {
      SerialPrintln(F("Unit flash aborted by /stop — unit left in twiboot"));
      return UnitFlashResult::Aborted;
    }
    uint16_t flashAddr = (uint16_t)(pageIndex * TWIBOOT_PAGE_SIZE);
    if (!flashAndVerifyPage(i2cAddress, flashAddr,
                            image + pageIndex * TWIBOOT_PAGE_SIZE)) {
      SerialPrintf("Unit flash FAILED at page 0x%04x — unit left in twiboot\n",
                   flashAddr);
      return UnitFlashResult::PageFailed;
    }
  }

  if (twibootExit(i2cAddress) != 0) return UnitFlashResult::ExitFailed;

  // Give the fresh sketch a couple of seconds to boot, then verify it
  // answers. twiboot's exit is a direct jump_to_app(), not a reset —
  // CMD_REBOOT gives the new sketch a clean watchdog restart (fresh
  // peripherals/MCUSR, DIP + EEPROM address re-read; v1 #113).
  delay(2000);
  Wire.beginTransmission((uint8_t)i2cAddress);
  if (Wire.endTransmission() != 0) {
    SerialPrintf("Unit 0x%02x not responding post-flash\n", i2cAddress);
    return UnitFlashResult::PostBootSilent;
  }
  Wire.beginTransmission((uint8_t)i2cAddress);
  Wire.write((uint8_t)SFP_CMD_REBOOT);
  Wire.endTransmission();
  SerialPrintf("Unit 0x%02x flashed (%u bytes) — sent CMD_REBOOT\n",
               i2cAddress, (unsigned)len);
  return UnitFlashResult::Ok;
}
