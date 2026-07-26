// FollowerBus.cpp — I2C unit bus glue (#298). Contract + caller rules in
// FollowerBus.h; straight port of v1's ServiceFlapFunctions.ino +
// ServiceFirmwareFunctions.ino onto the v2 UnitFacts model (blocking
// transactions and timing preserved — the Nanos are unchanged v1 hardware).

#include "FollowerBus.h"

#include <Wire.h>

#include "BootHomePlan.h"   // pure batched boot-home target selection (#309)
#include "BuildVersion.h"  // GIT_REV + BUNDLED_UNIT_REV (build_assets.py)
#include "DisplayWidth.h"
#include "FollowerConfig.h"
#include "HeartbeatPolicy.h"  // pure heartbeat miss/schedule logic (#310)
#include "RenderStagger.h"  // sub-frame inrush stagger (#324)
#include "SplitFlapProtocol.h"
#include "TwibootProtocol.h"
#include "UnitAssets.h"  // UNIT_FIRMWARE_BIN (build_assets.py)
#include "UnitProtocolHelpers.h"

UnitFacts unitFacts[UNITS_AMOUNT];
int displayWidth = UNITS_AMOUNT;
int detectedUnitCount = 0;
ReflashProgress reflashProgress;

static const char letters[] = SFP_ALPHABET;

// v1 #88: probes must never land inside a twiboot window.
static uint32_t probeInhibitUntilMs = 0;

uint32_t busProbeInhibitedUntilMs() { return probeInhibitUntilMs; }
void busArmProbeInhibit(uint32_t untilMs) { probeInhibitUntilMs = untilMs; }

// Freshness bookkeeping (miss counter / stale latch / lastSeenMs) is the pure
// heartbeatApply() in HeartbeatPolicy.h — same as the Master, copy policy.

// Delay between an opcode write and the read-back clocking (v1 value).
#define UNIT_RESPONSE_SETTLE_MS 2
// How long a segment write waits for the row to stop before assuming a
// unit is physically stuck (v1 value).
#define SHOW_STUCK_TIMEOUT_MS 30000UL

static int toI2cAddress(int unitIndex) {
  return SFP_I2C_ADDRESS_BASE + unitIndex;
}

void busInit() {
#if SERIAL_ENABLE == false
  // ESP-01: SDA=GPIO1(TX), SCL=GPIO3(RX) — v1 hardware truth; the Wire
  // buffer is bumped to 256 via -DI2C_BUFFER_LENGTH for twiboot's
  // 132-byte page writes.
  Wire.begin(1, 3);
#endif
}

// Bus health counters (#306): sketch-protocol read transactions and their
// failures since boot, surfaced in /cluster/health so a curl-only operator
// can see a flaky row. Bumped only by queryUnit (twiboot page writes and the
// bus-scan probe stay out, matching the master's i2cTx/i2cErr semantics).
static uint32_t busTxCount = 0;
static uint32_t busErrCount = 0;
uint32_t followerBusTxCount() { return busTxCount; }
uint32_t followerBusErrCount() { return busErrCount; }

// Since-boot minimum free heap (#306). ESP8266 has no built-in min-heap
// accessor, so track it: followerDiagTick() folds the current heap each loop
// pass, and the getter folds once more in case a tick lagged behind a spike.
static uint32_t minHeapBytes = 0xFFFFFFFFUL;
void followerDiagTick() {
  uint32_t h = ESP.getFreeHeap();
  if (h < minHeapBytes) minHeapBytes = h;
}
uint32_t followerMinHeap() {
  followerDiagTick();
  return minHeapBytes;
}

// Shared opcode-write-then-read-back transaction (v1 #154 helper).
static bool queryUnit(int i2cAddress, uint8_t opcode, uint8_t* buf,
                      uint8_t n) {
  busTxCount++;
  Wire.beginTransmission(i2cAddress);
  Wire.write(opcode);
  if (Wire.endTransmission() != 0) {
    busErrCount++;
    return false;
  }
  delay(UNIT_RESPONSE_SETTLE_MS);
  uint8_t got = Wire.requestFrom((uint8_t)i2cAddress, n);
  if (got != n) {
    while (Wire.available()) Wire.read();
    busErrCount++;
    return false;
  }
  for (uint8_t i = 0; i < n; i++) buf[i] = Wire.read();
  return true;
}

static bool readUnitStatus(int i2cAddress, UnitStatus& out) {
  uint8_t buf[8];
  if (!queryUnit(i2cAddress, (uint8_t)SFP_CMD_GET_STATUS, buf, 8)) {
    return false;
  }
  out.flags = buf[0];
  out.mcusrAtBoot = buf[1];
  out.lifetimeBrownoutCount = buf[2];
  out.lifetimeWatchdogCount = buf[3];
  out.uptimeSeconds = ((uint16_t)buf[4] << 8) | (uint16_t)buf[5];
  out.badCommandCount = buf[6];
  out.lastHomingStepCount = (uint16_t)buf[7] << 4;
  return true;
}

static bool readUnitOffset(int i2cAddress, int16_t& out) {
  uint8_t buf[2];
  if (!queryUnit(i2cAddress, (uint8_t)SFP_CMD_GET_OFFSET, buf, 2)) {
    return false;
  }
  out = (int16_t)((uint16_t)buf[0] | ((uint16_t)buf[1] << 8));
  return true;
}

static bool readUnitOdometer(int i2cAddress, uint32_t& out) {
  uint8_t buf[5];
  if (!queryUnit(i2cAddress, (uint8_t)SFP_CMD_GET_ODOMETER, buf, 5)) {
    return false;
  }
  return odometerReadbackValid(buf, out);
}

// Supply-Vcc / free-RAM / commanded-position diagnostics (#306) — same shared
// UnitVitals.h packet and checksum guard the master reads; pre-vitals firmware
// fails the checksum and stays vitalsValid=false.
static bool readUnitVitals(int i2cAddress, UnitVitals& out) {
  uint8_t buf[VITALS_REPLY_LEN];
  if (!queryUnit(i2cAddress, (uint8_t)SFP_CMD_GET_VITALS, buf, VITALS_REPLY_LEN)) {
    return false;
  }
  return vitalsReadbackValid(buf, out);
}

static void refreshUnitVitals(UnitFacts& fact, int i2cAddress) {
  fact.vitalsValid = false;
  UnitVitals v;
  if (!readUnitVitals(i2cAddress, v)) return;
  fact.vitals = v;
  fact.vitalsValid = true;
}

// New-measurement diagnostics (#365): same shared UnitExtDiag.h packet and
// checksum guard the master reads; pre-ext-diag firmware fails the checksum
// and stays extDiagValid=false.
static bool readUnitExtDiag(int i2cAddress, UnitExtDiag& out) {
  uint8_t buf[EXT_DIAG_REPLY_LEN];
  if (!queryUnit(i2cAddress, (uint8_t)SFP_CMD_GET_EXT_DIAG, buf, EXT_DIAG_REPLY_LEN)) {
    return false;
  }
  return extDiagReadbackValid(buf, out);
}

// Folds an ext-diag read into the slot; clears extDiagValid first so a unit
// that stops answering (or was reflashed to pre-ext-diag firmware) never
// keeps serving a stale reading (same discipline as refreshUnitVitals).
static void refreshUnitExtDiag(UnitFacts& fact, int i2cAddress) {
  fact.extDiagValid = false;
  UnitExtDiag d;
  if (!readUnitExtDiag(i2cAddress, d)) return;
  fact.extDiag = d;
  fact.extDiagValid = true;
}

// Across-power-cycle health (#406): same shared UnitLifetime.h packet and
// guard the master reads, so both rows report identically. Pre-lifetime
// firmware answers short and fails the length check.
static bool readUnitLifetime(int i2cAddress, UnitLifetimeFacts& out) {
  uint8_t buf[LIFETIME_REPLY_LEN];
  if (!queryUnit(i2cAddress, (uint8_t)SFP_CMD_GET_LIFETIME, buf, LIFETIME_REPLY_LEN)) {
    return false;
  }
  return lifetimeReadbackValid(buf, LIFETIME_REPLY_LEN, out);
}

// Folds a lifetime read into the slot; clears lifetimeValid first so a unit
// that stops answering (or was reflashed to pre-lifetime firmware) never
// keeps serving a stale record (same discipline as refreshUnitExtDiag).
static void refreshUnitLifetime(UnitFacts& fact, int i2cAddress) {
  fact.lifetimeValid = false;
  UnitLifetimeFacts lt;
  if (!readUnitLifetime(i2cAddress, lt)) return;
  fact.lifetime = lt;
  fact.lifetimeValid = true;
}

// v1 #140 rule: reject non-printables and the two JSON-structural chars at
// the I2C boundary — the version string is emitted raw into JSON.
static bool readUnitVersion(int i2cAddress, char* out) {
  out[0] = '\0';
  uint8_t buf[8];
  if (!queryUnit(i2cAddress, (uint8_t)SFP_CMD_GET_VERSION, buf, 8)) {
    return false;
  }
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

static bool readUnitDisplayedLetter(int i2cAddress, int& out) {
  uint8_t buf[2];
  if (!queryUnit(i2cAddress, (uint8_t)SFP_CMD_GET_LETTER, buf, 2)) {
    return false;
  }
  if (!letterReadbackValid(buf[0], buf[1], (uint8_t)SFP_FLAP_AMOUNT)) {
    return false;
  }
  out = buf[0];
  return true;
}

// Twiboot chipinfo probe — safe against a sketch-running unit (v1 note:
// the patched Unit.ino ignores writes of length != 2).
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

void busProbe() {
#if SERIAL_ENABLE == false
  SerialPrintln(F("Scanning I2C bus for units..."));
  detectedUnitCount = 0;
  int states[UNITS_AMOUNT];
  for (int i = 0; i < UNITS_AMOUNT; i++) {
    unitFacts[i] = UnitFacts{};
    states[i] = 0;
    int i2cAddress = toI2cAddress(i);
    Wire.beginTransmission(i2cAddress);
    if (Wire.endTransmission() != 0) continue;

    bool inBootloader = isUnitInBootloader(i2cAddress);
    unitFacts[i].state = inBootloader ? 2 : 1;
    states[i] = unitFacts[i].state;
    detectedUnitCount++;
    if (inBootloader) continue;

    if (readUnitVersion(i2cAddress, unitFacts[i].version)) {
      unitFacts[i].fwStatus =
          unitFwStatusFromRev(unitFacts[i].version, BUNDLED_UNIT_REV);
    }
    int16_t offset;
    if (readUnitOffset(i2cAddress, offset)) {
      unitFacts[i].offset = offset;
      unitFacts[i].offsetValid = true;
    }
    uint32_t odometer;
    if (readUnitOdometer(i2cAddress, odometer)) {
      unitFacts[i].odometer = odometer;
      unitFacts[i].odometerValid = true;
    }
    refreshUnitVitals(unitFacts[i], i2cAddress);
    // New-measurement diagnostics ride the probe too (#365); pre-ext-diag
    // firmware fails the checksum and stays extDiagValid=false.
    refreshUnitExtDiag(unitFacts[i], i2cAddress);
    // Lifetime health rides the probe too (#406); pre-lifetime firmware
    // fails the length check and stays lifetimeValid=false.
    refreshUnitLifetime(unitFacts[i], i2cAddress);
  }
  displayWidth = computeDisplayWidth(states, UNITS_AMOUNT);
  SerialPrint(F("I2C scan complete. Detected "));
  SerialPrint(detectedUnitCount);
  SerialPrint(F(" unit(s). Row width: "));
  SerialPrintln(displayWidth);
#endif
}

bool busPollHealthOne(int i) {
#if SERIAL_ENABLE == false
  if (unitFacts[i].state != 1) {
    unitFacts[i].statusValid = false;
    return false;
  }
  UnitStatus s;
  bool ok = readUnitStatus(toI2cAddress(i), s);
  if (ok) {
    unitFacts[i].status = s;
    unitFacts[i].statusValid = true;
  } else {
    unitFacts[i].statusValid = false;
  }
  uint32_t odometer;
  if (readUnitOdometer(toI2cAddress(i), odometer)) {
    unitFacts[i].odometer = odometer;
    unitFacts[i].odometerValid = true;
  }
  refreshUnitVitals(unitFacts[i], toI2cAddress(i));
  // New-measurement diagnostics refresh on the same cadence (#365); not
  // charged to bus error attribution — same as odometer/vitals above, only
  // the CMD_GET_STATUS read above is the liveness signal.
  refreshUnitExtDiag(unitFacts[i], toI2cAddress(i));
  // Lifetime health refreshes on the same cadence (#406) — a failed homing
  // must not wait for the next probe to surface.
  refreshUnitLifetime(unitFacts[i], toI2cAddress(i));
  return ok;  // CMD_GET_STATUS liveness signal for the heartbeat (#310)
#else
  (void)i;
  return false;
#endif
}

void busPollHealth() {
#if SERIAL_ENABLE == false
  if (reflashInProgress(reflashProgress)) return;
  uint32_t now = millis();
  for (int i = 0; i < UNITS_AMOUNT; i++) {
    bool ok = busPollHealthOne(i);
    heartbeatApply(unitFacts[i], ok, now, HEARTBEAT_MISS_THRESHOLD);
  }
#endif
}

void followerHeartbeatTick() {
#if SERIAL_ENABLE == false
  static uint32_t lastMs = 0;
  static int slot = 0;
  uint32_t now = millis();
  if (now - lastMs < HEARTBEAT_TICK_MS) return;
  lastMs = now;
  // Opportunistic + low priority (#310): never touch the bus while a unit may
  // be in its twiboot window (v1 #88) or a reflash is streaming.
  if ((int32_t)(now - busProbeInhibitedUntilMs()) < 0) return;
  if (reflashInProgress(reflashProgress)) return;
  if (displayWidth <= 0) return;
  int i = slot;
  slot = heartbeatNextSlot(slot, displayWidth);
  bool ok = busPollHealthOne(i);
  heartbeatApply(unitFacts[i], ok, millis(), HEARTBEAT_MISS_THRESHOLD);
#endif
}

static int translateLetterToInt(char letterChar) {
  for (int i = 0; i < SFP_FLAP_AMOUNT; i++) {
    if (letterChar == letters[i]) return i;
  }
  return -1;
}

static int writeToUnit(int unitIndex, int letter, int speed) {
  Wire.beginTransmission(toI2cAddress(unitIndex));
  Wire.write(letter);
  Wire.write(speed);
  return Wire.endTransmission();
}

// 0 idle, 1 rotating, -1 offline (v1 checkIfMoving, incl. the wake-up ping).
static int checkIfMoving(int unitIndex) {
  int i2cAddress = toI2cAddress(unitIndex);
  Wire.requestFrom(i2cAddress, 1, 1);
  int active = Wire.read();
  if (active == -1) {
    Wire.beginTransmission(i2cAddress);
    Wire.endTransmission();
  }
  return active;
}

static bool isRowMoving() {
  for (int i = 0; i < UNITS_AMOUNT; i++) {
    if (unitFacts[i].state != 1) continue;
    if (checkIfMoving(i) == 1) return true;
  }
  return false;
}

// v1 waitForDisplayToStop, minus the /stop abort (this firmware has no
// local producers and serves no /stop — the stuck timeout is the bound).
static void waitForRowToStop() {
  uint32_t waitStart = millis();
  while (isRowMoving()) {
    if (millis() - waitStart > SHOW_STUCK_TIMEOUT_MS) {
      SerialPrintln(F("Row-stop wait timed out — a unit may be stuck"));
      break;
    }
    delay(100);
  }
}

static int convertWebSpeed(int webSpeed) {
  webSpeed = constrain(webSpeed, 1, 100);
  return map(webSpeed, 1, 100, MIN_SPEED, MAX_SPEED);
}

void busShowSegment(const String& segment, int webSpeed) {
#if SERIAL_ENABLE == false
  const int width = displayWidth;
  const int speed = convertWebSpeed(webSpeed);
  // Segments arrive pre-positioned from the leader: pad/truncate to the
  // probed width, no alignment pass.
  String frame = segment;
  while ((int)frame.length() < width) frame += ' ';

  waitForRowToStop();

  int commanded[UNITS_AMOUNT];
  for (int i = 0; i < UNITS_AMOUNT; i++) commanded[i] = -1;

  int commandedCount = 0;
  for (int i = 0; i < width; i++) {
    if (unitFacts[i].state != 1) continue;
    int letter = translateLetterToInt(frame[i]);
    if (letter < 0) continue;  // char not on the drum: leave the unit be
    // #324: spread the flap inrush — pause before opening each new group so a
    // full row's steppers don't spin up at once and brown out the rail.
    if (renderStaggerShouldSettle(commandedCount, RENDER_STAGGER_BATCH)) {
      delay(RENDER_STAGGER_SETTLE_MS);
    }
    writeToUnit(i, letter, speed);
    commanded[i] = letter;
    commandedCount++;
  }

  waitForRowToStop();

  // Closed-loop verification (v1 #106): read back, re-send once on mismatch.
  int resent = 0;
  for (int i = 0; i < UNITS_AMOUNT; i++) {
    if (commanded[i] < 0 || unitFacts[i].state != 1) continue;
    int shown;
    if (!readUnitDisplayedLetter(toI2cAddress(i), shown)) continue;
    if (shown == commanded[i]) continue;
    writeToUnit(i, commanded[i], speed);
    resent++;
  }
  if (resent > 0) waitForRowToStop();
#else
  SerialPrint(F("Row shows: \""));
  SerialPrint(segment);
  SerialPrint(F("\" speed "));
  SerialPrintln(webSpeed);
#endif
}

// --- single-unit ops ---------------------------------------------------------------

int busWriteOffset(uint8_t i2cAddress, int16_t value) {
  uint8_t enc[2];
  maintEncodeOffsetLE(value, enc);
  Wire.beginTransmission(i2cAddress);
  Wire.write((uint8_t)SFP_CMD_SET_OFFSET);
  Wire.write(enc[0]);
  Wire.write(enc[1]);
  return Wire.endTransmission();
}

int busJog(uint8_t i2cAddress, int steps) {
  Wire.beginTransmission(i2cAddress);
  Wire.write((uint8_t)SFP_CMD_JOG);
  Wire.write(maintEncodeJogByte(steps));
  return Wire.endTransmission();
}

int busHome(uint8_t i2cAddress) {
  Wire.beginTransmission(i2cAddress);
  Wire.write((uint8_t)SFP_CMD_HOME);
  return Wire.endTransmission();
}

int busIdentify(uint8_t i2cAddress) {
  Wire.beginTransmission(i2cAddress);
  Wire.write((uint8_t)SFP_CMD_IDENTIFY);
  return Wire.endTransmission();
}

int busResetOdometer(uint8_t i2cAddress) {
  Wire.beginTransmission(i2cAddress);
  Wire.write((uint8_t)SFP_CMD_RESET_ODOMETER);
  return Wire.endTransmission();
}

int busRebootToBootloader(uint8_t i2cAddress) {
  Wire.beginTransmission(i2cAddress);
  Wire.write((uint8_t)SFP_CMD_ENTER_BOOTLOADER);
  return Wire.endTransmission();
}

// Soft watchdog reset — stays in sketch mode (v1 #47/#113).
static int rebootUnit(uint8_t i2cAddress) {
  Wire.beginTransmission(i2cAddress);
  Wire.write((uint8_t)SFP_CMD_REBOOT);
  return Wire.endTransmission();
}

int busStartSelfTest(uint8_t i2cAddress) {
  Wire.beginTransmission(i2cAddress);
  Wire.write((uint8_t)SFP_CMD_START_SELF_TEST);
  return Wire.endTransmission();
}

bool busReadSelfTest(uint8_t i2cAddress, UnitSelfTestReading& out) {
  uint8_t buf[9];
  if (!queryUnit(i2cAddress, (uint8_t)SFP_CMD_GET_SELF_TEST, buf, 9)) {
    return false;
  }
  return selfTestReadbackValid(buf, out);
}

// --- twiboot flash (v1 ServiceFirmwareFunctions port) --------------------------------

static uint8_t twibootAddr = 0;
static String flashError;

static int twibootPing() {
  Wire.beginTransmission(twibootAddr);
  Wire.write((uint8_t)TWIBOOT_CMD_WAIT);
  return Wire.endTransmission();
}

static int twibootExit() {
  Wire.beginTransmission(twibootAddr);
  Wire.write((uint8_t)TWIBOOT_CMD_SWITCH_APPLICATION);
  Wire.write((uint8_t)TWIBOOT_BOOTTYPE_APPLICATION);
  return Wire.endTransmission();
}

static bool twibootVerifyChip() {
  Wire.beginTransmission(twibootAddr);
  Wire.write((uint8_t)TWIBOOT_CMD_ACCESS_MEMORY);
  Wire.write((uint8_t)TWIBOOT_MEMTYPE_CHIPINFO);
  Wire.write((uint8_t)0x00);
  Wire.write((uint8_t)0x00);
  if (Wire.endTransmission(false) != 0) {
    flashError = F("Wire endTransmission failed reading chipinfo");
    return false;
  }
  uint8_t got = Wire.requestFrom(twibootAddr, (uint8_t)8);
  if (got != 8) {
    flashError = String(F("Chipinfo read returned ")) + got + F(" bytes");
    return false;
  }
  uint8_t sig0 = Wire.read(), sig1 = Wire.read(), sig2 = Wire.read();
  uint8_t pageSize = Wire.read();
  Wire.read(); Wire.read();
  Wire.read(); Wire.read();
  if (!isAtmega328pSignature(sig0, sig1, sig2)) {
    flashError = F("Unexpected chip signature");
    return false;
  }
  if (pageSize != TWIBOOT_PAGE_SIZE) {
    flashError = F("Unexpected page size");
    return false;
  }
  return true;
}

// Spin-poll twiboot with CMD_WAIT until it ACKs (async SPM write done).
static bool twibootWaitReady(uint16_t timeoutMs) {
  uint32_t deadline = millis() + timeoutMs;
  while ((int32_t)(millis() - deadline) < 0) {
    if (twibootPing() == 0) return true;
    delay(1);
  }
  return false;
}

static bool twibootReadFlashPage(uint16_t flashAddr, uint8_t* out) {
  Wire.beginTransmission(twibootAddr);
  Wire.write((uint8_t)TWIBOOT_CMD_ACCESS_MEMORY);
  Wire.write((uint8_t)TWIBOOT_MEMTYPE_FLASH);
  Wire.write((uint8_t)((flashAddr >> 8) & 0xFF));
  Wire.write((uint8_t)(flashAddr & 0xFF));
  if (Wire.endTransmission(false) != 0) return false;
  uint8_t got = Wire.requestFrom(twibootAddr, (uint8_t)TWIBOOT_PAGE_SIZE);
  if (got != TWIBOOT_PAGE_SIZE) {
    while (Wire.available()) Wire.read();
    return false;
  }
  for (int i = 0; i < TWIBOOT_PAGE_SIZE; i++) out[i] = Wire.read();
  return true;
}

static int twibootWriteFlashPage(uint16_t flashAddr, const uint8_t* page) {
  Wire.beginTransmission(twibootAddr);
  Wire.write((uint8_t)TWIBOOT_CMD_ACCESS_MEMORY);
  Wire.write((uint8_t)TWIBOOT_MEMTYPE_FLASH);
  Wire.write((uint8_t)((flashAddr >> 8) & 0xFF));
  Wire.write((uint8_t)(flashAddr & 0xFF));
  for (int i = 0; i < TWIBOOT_PAGE_SIZE; i++) Wire.write(page[i]);
  return Wire.endTransmission();
}

// Write + read-back verify with one rewrite attempt (v1 #110).
static bool flashAndVerifyPage(const uint8_t* page, uint16_t addr) {
  for (int attempt = 0; attempt < 2; attempt++) {
    if (!twibootWaitReady(100)) {
      flashError = F("twiboot not ready before page");
      return false;
    }
    if (twibootWriteFlashPage(addr, page) != 0) {
      flashError = F("page write failed");
      return false;
    }
    if (!twibootWaitReady(50)) {
      flashError = F("twiboot stuck busy after page");
      return false;
    }
    uint8_t readBuf[TWIBOOT_PAGE_SIZE];
    if (!twibootReadFlashPage(addr, readBuf)) {
      flashError = F("verify read failed");
      return false;
    }
    if (memcmp(readBuf, page, TWIBOOT_PAGE_SIZE) == 0) return true;
  }
  flashError = F("verify mismatch persisted");
  return false;
}

// Streams the PROGMEM-embedded unit firmware to one unit's twiboot.
static bool flashUnitFromProgmem(uint8_t i2cAddress) {
  twibootAddr = i2cAddress;
  flashError = "";

  if (!isUnitInBootloader((int)i2cAddress)) {
    if (busRebootToBootloader(i2cAddress) != 0) {
      flashError = F("unit did not ack enter-bootloader");
      return false;
    }
    delay(TWIBOOT_STARTUP_MS);
  }

  bool live = false;
  for (int attempt = 0; attempt < 5; attempt++) {
    if (twibootPing() == 0) {
      live = true;
      break;
    }
    delay(100);
  }
  if (!live) {
    flashError = F("twiboot not responding");
    return false;
  }
  if (!twibootVerifyChip()) return false;

  size_t pageCount = UNIT_FIRMWARE_BIN_LEN / TWIBOOT_PAGE_SIZE;
  uint8_t pageBuf[TWIBOOT_PAGE_SIZE];
  for (size_t pageIndex = 0; pageIndex < pageCount; pageIndex++) {
    memcpy_P(pageBuf, UNIT_FIRMWARE_BIN + pageIndex * TWIBOOT_PAGE_SIZE,
             TWIBOOT_PAGE_SIZE);
    if (!flashAndVerifyPage(pageBuf,
                            (uint16_t)(pageIndex * TWIBOOT_PAGE_SIZE))) {
      SerialPrint(F("Unit flash failed: "));
      SerialPrintln(flashError);
      return false;
    }
  }

  if (twibootExit() != 0) {
    flashError = F("exit bootloader failed");
    return false;
  }
  // Let the sketch boot, then a clean watchdog restart (v1 #113: twiboot's
  // exit is a jump, not a reset).
  delay(2000);
  Wire.beginTransmission(i2cAddress);
  if (Wire.endTransmission() == 0) rebootUnit(i2cAddress);
  return true;
}

// Polls a just-flashed batch until online + homed (v1 #138 throttle).
static void waitForBatchIdle(const uint8_t* addrs, int count,
                             uint32_t timeoutMs) {
  delay(1000);
  uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    bool allIdle = true;
    for (int k = 0; k < count; k++) {
      if (checkIfMoving(addrs[k] - SFP_I2C_ADDRESS_BASE) != 0) {
        allIdle = false;
        break;
      }
    }
    if (allIdle) return;
    delay(100);
  }
}

// Staggered boot-home (#309): the units boot UNHOMED, so the follower homes
// the ones that still report unhomed in bounded batches with a rail-settle
// between them — a whole row's steppers don't spike the shared rail at once
// (the #305 brownout class). Targets only unhomed sketch units, so it re-homes
// nothing already good. loop()-blocking (setup() only, like busProbe).
void followerBootHome() {
#if SERIAL_ENABLE == false
  uint8_t targets[UNITS_AMOUNT];
  int n = bootHomeCollectTargets(unitFacts, displayWidth, SFP_I2C_ADDRESS_BASE,
                                 targets);
  if (n == 0) return;
  SerialPrint(F("boot-home: staggering "));
  SerialPrint(n);
  SerialPrintln(F(" unit(s)"));
  for (int i = 0; i < n; i += BOOT_HOME_BATCH_SIZE) {
    uint8_t batch[BOOT_HOME_BATCH_SIZE];
    int batchN = 0;
    for (int j = i; j < n && batchN < BOOT_HOME_BATCH_SIZE; j++) {
      busHome(targets[j]);
      batch[batchN++] = targets[j];
    }
    waitForBatchIdle(batch, batchN, BOOT_HOME_BATCH_TIMEOUT_MS);
    delay(BOOT_HOME_SETTLE_MS);
  }
  busPollHealth();  // reflect the now-homed state (also re-stamps freshness)
#endif
}

// Flash every bootloader-mode unit in the CURRENT facts, batched. Updates
// facts in place for successes (v1 #120 rule: the streamed image IS the
// bundle, page-verified — don't re-read over I2C and risk pinning twiboot).
static void flashBootloaderUnits() {
  uint8_t batch[REFLASH_BATCH_SIZE];
  int batchCount = 0;
  for (int i = 0; i < UNITS_AMOUNT; i++) {
    if (unitFacts[i].state != 2) continue;
    uint8_t addr = (uint8_t)toI2cAddress(i);
    reflashProgressUnitStart(reflashProgress, addr);
    bool ok = flashUnitFromProgmem(addr);
    reflashProgressUnitResult(reflashProgress, ok);
    if (ok) {
      unitFacts[i].state = 1;
      strncpy(unitFacts[i].version, BUNDLED_UNIT_REV, 8);
      unitFacts[i].version[8] = '\0';
      unitFacts[i].fwStatus = 0;
      batch[batchCount++] = addr;
    }
    if (batchCount >= REFLASH_BATCH_SIZE) {
      reflashProgressSettling(reflashProgress);
      waitForBatchIdle(batch, batchCount, REFLASH_BATCH_SETTLE_MS);
      batchCount = 0;
    }
  }
  if (batchCount > 0) {
    reflashProgressSettling(reflashProgress);
    waitForBatchIdle(batch, batchCount, REFLASH_BATCH_SETTLE_MS);
  }
}

void busAutoInstallBootloaderUnits() {
#if SERIAL_ENABLE == false
  uint8_t targets[UNITS_AMOUNT];
  int n = reflashCollectFlashTargets(unitFacts, UNITS_AMOUNT,
                                     SFP_I2C_ADDRESS_BASE, targets);
  if (n == 0) return;
  reflashProgressBegin(reflashProgress, n);
  flashBootloaderUnits();
  reflashProgressFinish(reflashProgress, false);
#endif
}

void busAutoUpdateOutdatedUnits() {
#if SERIAL_ENABLE == false
  uint8_t targets[UNITS_AMOUNT];
  int n = reflashCollectOutdatedTargets(unitFacts, UNITS_AMOUNT,
                                        SFP_I2C_ADDRESS_BASE, targets);
  if (n == 0) return;
  for (int k = 0; k < n; k++) busRebootToBootloader(targets[k]);
  delay(TWIBOOT_STARTUP_MS);
  busProbe();  // reflash-internal probe: pinned units are flashed right away
  busAutoInstallBootloaderUnits();
#endif
}

void busRunReflashJob() {
#if SERIAL_ENABLE == false
  SerialPrintln(F("Unit reflash starting (throttled)..."));
  uint8_t targets[UNITS_AMOUNT];
  int rebooted = reflashCollectRebootTargets(unitFacts, UNITS_AMOUNT,
                                             SFP_I2C_ADDRESS_BASE, targets);
  for (int k = 0; k < rebooted; k++) busRebootToBootloader(targets[k]);
  if (rebooted > 0) delay(TWIBOOT_STARTUP_MS);
  busProbe();  // reflash-internal probe (#205 exception to the inhibit)
  uint8_t flashTargets[UNITS_AMOUNT];
  int n = reflashCollectFlashTargets(unitFacts, UNITS_AMOUNT,
                                     SFP_I2C_ADDRESS_BASE, flashTargets);
  reflashProgressBegin(reflashProgress, n);
  flashBootloaderUnits();
  reflashProgressFinish(reflashProgress, false);
  busPollHealth();
  // Staggered boot-home of the just-flashed units (#309): a reflashed unit
  // reboots UNHOMED, so without this the next cluster render would home the
  // whole row at once — the #305 inrush class. Targets only unhomed units.
  followerBootHome();
  SerialPrintln(F("Unit reflash complete."));
#endif
}
