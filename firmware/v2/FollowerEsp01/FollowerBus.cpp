// FollowerBus.cpp — I2C unit bus glue (#298). Contract + caller rules in
// FollowerBus.h; straight port of v1's ServiceFlapFunctions.ino +
// ServiceFirmwareFunctions.ino onto the v2 UnitFacts model (blocking
// transactions and timing preserved — the Nanos are unchanged v1 hardware).

#include "FollowerBus.h"

#include <Wire.h>

#include "BootHomePlan.h"   // pure batched boot-home target selection (#309)
#include "BuildVersion.h"  // GIT_REV + BUNDLED_UNIT_REV (build_assets.py)
#include "FollowerBusRecovery.h"  // pure row-wide bus-death policy (#488)
#include "DisplayWidth.h"
#include "FollowerConfig.h"
#include "FollowerWifi.h"  // followerRadioBusy (#505)
#include "HeartbeatPolicy.h"  // pure heartbeat miss/schedule logic (#310)
#include "MotionBudget.h"  // motion admission (#505)
#include "RenderStagger.h"  // sub-frame inrush stagger (#324)
#include "SplitFlapProtocol.h"
#include "TwibootProtocol.h"
#include "UnitAssets.h"  // UNIT_FIRMWARE_BIN (build_assets.py)
#include "UnitProtocolHelpers.h"
#include "UnitRescuePolicy.h"  // runtime rescue of lost units (#498)
#include "BootUpdatePlan.h"   // #499 decision logic (includes BootUpdateReport)

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
  // #488: a warm reboot (OTA, /reboot) leaves the Nanos powered, so one held
  // SDA mid-byte survives it and the boot probe finds nothing. Clock it free
  // before anything scans.
  uint8_t status = Wire.status();
  if (status != 0) {
    SerialPrint(F("bus: line held at boot, state "));
    SerialPrintln(status);
  }
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

// Row-wide bus-death recovery (#488): policy in FollowerBusRecovery.h. The
// last frame written is kept so a recovered bus re-shows it — without that a
// text-mode row stays frozen on whatever the dead bus half-rendered until the
// leader happens to send new content.
static_assert(UNITS_AMOUNT <= 32, "BusRecoveryState.failMask is 32 bits");
static BusRecoveryState busRecovery;
static bool reshowPending = false;
static String lastFrame;
static int lastFrameSpeed = 0;
static bool lastFrameValid = false;
// Attempt lines logged per episode; a bus held for good then goes quiet
// instead of evicting the 2 KB ring (the #436 flood lesson).
#define BUS_RECOVERY_LOGGED_ATTEMPTS 3

const BusRecoveryState& followerBusRecovery() { return busRecovery; }

#if SERIAL_ENABLE == false
// Only drivable units feed the detector: busPollHealthOne() returns false for
// the rest without touching the bus, which is no evidence either way. The
// reshow is staged, not run here: a render blocks up to SHOW_STUCK_TIMEOUT_MS.
static void observeLiveness(int i, bool ok) {
  BusRecoveryEvent e =
      busRecoveryObserve(busRecovery, i, ok, millis(), displayWidth);
  if (e == BusRecoveryEvent::WentDead) {
    SerialPrintln(F("bus: every unit stopped answering — recovering the I2C bus"));
  } else if (e == BusRecoveryEvent::Recovered) {
    SerialPrint(F("bus: recovered after "));
    SerialPrint(busRecovery.lastDeadMs / 1000UL);
    SerialPrintln(F(" s — re-showing the last frame"));
    reshowPending = lastFrameValid;
  }
}

// Clocks a slave-held SDA free: Wire.status() reads up to 20 bits, releasing
// a Nano stuck mid-byte, and leaves both lines released so the next START
// resets every slave's TWI state machine. No Wire.begin() re-init — Twi::init
// re-registers core timers/tasks, and the pins never change mode. SCL held low
// is reported but cannot be fixed from this side: the Nano needs a power cycle.
static void followerBusRecoveryTick() {
  uint32_t now = millis();
  if (reshowPending) {
    reshowPending = false;
    busShowSegment(String(lastFrame), lastFrameSpeed);
  }
  if (!busRecoveryDue(busRecovery, now)) return;
  uint8_t status = Wire.status();
  busRecoveryNoteAttempt(busRecovery, now, status);
  if (busRecovery.attemptsThisEpisode <= BUS_RECOVERY_LOGGED_ATTEMPTS) {
    SerialPrint(F("bus: recovery attempt "));
    SerialPrint(busRecovery.attemptsThisEpisode);
    SerialPrint(F(", line state "));
    SerialPrintln(status);
  }
  // An empty row has no liveness reads to close the episode: re-probe
  // (quietly — this repeats every backoff step) and close it here.
  if (detectedUnitCount == 0) {
    busProbeQuiet(true);
    if (detectedUnitCount > 0) {
      SerialPrint(F("bus: re-probe found "));
      SerialPrint(detectedUnitCount);
      SerialPrintln(F(" unit(s)"));
      observeLiveness(0, true);
    }
  }
}
#endif

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

// No-argument mutations go out as opcode + ~opcode (#512, UnitWireContract.h):
// alone on the wire, the opcode byte is a complete command one bit flip away
// from a letter write or a poll. Units predating the guard drain the extra byte.
// Not for ENTER_BOOTLOADER — see its sender.
static void writeGuardedOpcode(uint8_t opcode) {
  Wire.write(opcode);
  Wire.write(noArgGuardByte(opcode));
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

// 8-byte payload + its #405 checksum, same shared guard the master uses.
static bool readUnitStatus(int i2cAddress, UnitStatus& out) {
  uint8_t buf[STATUS_REPLY_LEN];
  if (!queryUnit(i2cAddress, (uint8_t)SFP_CMD_GET_STATUS, buf, STATUS_REPLY_LEN)) {
    return false;
  }
  uint8_t p[STATUS_PAYLOAD_LEN];
  if (!statusReadbackValid(buf, STATUS_REPLY_LEN, p)) return false;
  out.flags = p[0];
  out.mcusrAtBoot = p[1];
  out.lifetimeBrownoutCount = p[2];
  out.lifetimeWatchdogCount = p[3];
  out.uptimeSeconds = ((uint16_t)p[4] << 8) | (uint16_t)p[5];
  out.badCommandCount = p[6];
  out.lastHomingStepCount = (uint16_t)p[7] << 4;
  return true;
}

static bool readUnitOffset(int i2cAddress, int16_t& out) {
  uint8_t buf[OFFSET_REPLY_LEN];
  if (!queryUnit(i2cAddress, (uint8_t)SFP_CMD_GET_OFFSET, buf, OFFSET_REPLY_LEN)) {
    return false;
  }
  return offsetReadbackValid(buf, OFFSET_REPLY_LEN, out);
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
static bool readUnitExtDiag(int i2cAddress, uint8_t* buf) {
  return queryUnit(i2cAddress, (uint8_t)SFP_CMD_GET_EXT_DIAG, buf,
                   EXT_DIAG_LINK_REPLY_LEN);
}

// Folds an ext-diag read into the slot (#502 link extension included); both
// valid flags clear first so a unit that stops answering (or was reflashed to
// pre-ext-diag firmware) never keeps serving a stale reading (same discipline
// as refreshUnitVitals).
static void refreshUnitExtDiag(UnitFacts& fact, int i2cAddress) {
  fact.extDiagValid = false;
  fact.linkValid = false;
  uint8_t buf[EXT_DIAG_LINK_REPLY_LEN];
  if (!readUnitExtDiag(i2cAddress, buf)) return;
  unitFactsFoldExtDiag(fact, buf);
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

// Boot-section integrity (BootIntegrity.h, #520): judges the unit's boot
// report on every health poll, same rules as the S3 master. The verdict
// clears first; the logged verdict lives outside the facts so a rescan does
// not repeat a finding.
static uint8_t bootVerdictLogged[UNITS_AMOUNT];

static void refreshUnitBootVerdict(UnitFacts& fact, int unitIndex) {
  fact.bootVerdict = BOOT_INTEGRITY_UNREAD;
  uint8_t i2cAddress = (uint8_t)toI2cAddress(unitIndex);
  BootUpdateReport r;
  if (!busReadBootInfo(i2cAddress, r)) return;
  fact.bootCrc32 = r.bootCrc32;
  fact.bootVerdict = bootIntegrityJudge(r, BOOT_CURRENT_CRC32);
  BootIntegrityEdge e =
      bootIntegrityEdge(bootVerdictLogged[unitIndex], fact.bootVerdict);
  bootVerdictLogged[unitIndex] = e.logged;
  if (!e.log) return;
  char logBuf[88];
  snprintf(logBuf, sizeof(logBuf),
           "Unit 0x%02x bootloader %s - crc32 %08lx (expected %08lx)",
           i2cAddress, bootIntegrityName(fact.bootVerdict),
           (unsigned long)r.bootCrc32, (unsigned long)BOOT_CURRENT_CRC32);
  SerialPrintln(logBuf);
}

// v1 #140 rule: reject non-printables and the two JSON-structural chars at
// the I2C boundary — the version string is emitted raw into JSON.
// Also yields the unit's SFP_PROTOCOL_VERSION (#405) — the one number saying
// which wire contract it speaks. This reply's shape is frozen forever.
static bool readUnitVersion(int i2cAddress, char* out, uint8_t& protocolOut) {
  out[0] = '\0';
  protocolOut = 0;
  uint8_t buf[VERSION_REPLY_LEN];
  if (!queryUnit(i2cAddress, (uint8_t)SFP_CMD_GET_VERSION, buf, VERSION_REPLY_LEN)) {
    return false;
  }
  UnitVersionPacket pkt;
  if (!versionReadbackValid(buf, VERSION_REPLY_LEN, pkt)) return false;
  uint8_t len = 0;
  for (; len < VERSION_REV_LEN; len++) {
    if (pkt.rev[len] == 0) break;
    if (pkt.rev[len] < 32 || pkt.rev[len] > 126) return false;
    if (pkt.rev[len] == '"' || pkt.rev[len] == '\\') return false;
  }
  if (len == 0) return false;
  for (uint8_t i = 0; i < len; i++) out[i] = pkt.rev[i];
  out[len] = '\0';
  protocolOut = pkt.protocolVersion;
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

void busProbe() { busProbeQuiet(false); }

void busProbeQuiet(bool quiet) {
#if SERIAL_ENABLE == false
  if (!quiet) SerialPrintln(F("Scanning I2C bus for units..."));
  // #468: async handlers (/units/health, /unit/offset, /settings) read
  // unitFacts / detectedUnitCount live, and the I2C reads below yield —
  // zeroing a slot up front and refilling it field by field would let a GET
  // landing mid-probe see the unit as absent/unversioned. Each slot is
  // therefore probed into a scratch struct and published with one
  // assignment (no yield inside a struct copy on this single-core part),
  // and the count is published once at the end.
  int detected = 0;
  int states[UNITS_AMOUNT];
  for (int i = 0; i < UNITS_AMOUNT; i++) {
    UnitFacts f{};
    states[i] = 0;
    int i2cAddress = toI2cAddress(i);
    Wire.beginTransmission(i2cAddress);
    if (Wire.endTransmission() != 0) {
      unitFacts[i] = f;
      continue;
    }

    bool inBootloader = isUnitInBootloader(i2cAddress);
    f.state = inBootloader ? 2 : 1;
    states[i] = f.state;
    detected++;
    if (inBootloader) {
      unitFacts[i] = f;
      continue;
    }

    uint8_t protocolVersion = 0;
    if (readUnitVersion(i2cAddress, f.version, protocolVersion)) {
      // Rev (a hash) and protocol version are both compared for EQUALITY
      // only — neither says "older", and different always means reflash.
      // BUNDLED_UNIT_REV_EQUIV widens "ours" to revs measured to build the
      // same machine code (#440).
      f.fwStatus = unitFwStatusFromRev(f.version, BUNDLED_UNIT_REV,
                                       BUNDLED_UNIT_REV_EQUIV);
      f.protocolVersion = protocolVersion;
      f.protocolKnown = true;
    }
    int16_t offset;
    if (readUnitOffset(i2cAddress, offset)) {
      f.offset = offset;
      f.offsetValid = true;
    }
    uint32_t odometer;
    if (readUnitOdometer(i2cAddress, odometer)) {
      f.odometer = odometer;
      f.odometerValid = true;
    }
    refreshUnitVitals(f, i2cAddress);
    // New-measurement diagnostics ride the probe too (#365); pre-ext-diag
    // firmware fails the checksum and stays extDiagValid=false.
    refreshUnitExtDiag(f, i2cAddress);
    // Lifetime health rides the probe too (#406); pre-lifetime firmware
    // fails the length check and stays lifetimeValid=false.
    refreshUnitLifetime(f, i2cAddress);
    refreshUnitBootVerdict(f, i);  // #520
    unitFacts[i] = f;
  }
  detectedUnitCount = detected;
  displayWidth = computeDisplayWidth(states, UNITS_AMOUNT);
  if (quiet) return;
  SerialPrint(F("I2C scan complete. Detected "));
  SerialPrint(detectedUnitCount);
  SerialPrint(F(" unit(s). Row width: "));
  SerialPrintln(displayWidth);
#endif
}

// Per-unit reset-counter baselines (UnitHealth.h). Outside the facts so a
// probe rescan, which rebuilds every slot, does not re-baseline a unit that
// reset; only a follower reboot does.
static UnitResetBaseline resetBaselines[UNITS_AMOUNT];

bool busPollHealthOne(int i) {
#if SERIAL_ENABLE == false
  if (!unitDrivable(unitFacts[i])) {  // #405
    unitFacts[i].statusValid = false;
    unitFacts[i].bootVerdict = BOOT_INTEGRITY_UNREAD;
    return false;
  }
  // #468: same publish-on-complete rule as busProbe() above — the reads
  // below yield, so mutate a scratch copy (seeded from the published slot:
  // a poll updates, it never resets) and publish with one assignment.
  UnitFacts f = unitFacts[i];
  UnitStatus s;
  bool ok = readUnitStatus(toI2cAddress(i), s);
  if (ok) {
    f.status = s;
    f.statusValid = true;
    f.resetSeen = unitResetBaselineFold(
        resetBaselines[i], s.lifetimeBrownoutCount, s.lifetimeWatchdogCount);
  } else {
    f.statusValid = false;
  }
  uint32_t odometer;
  if (readUnitOdometer(toI2cAddress(i), odometer)) {
    f.odometer = odometer;
    f.odometerValid = true;
  }
  refreshUnitVitals(f, toI2cAddress(i));
  // New-measurement diagnostics refresh on the same cadence (#365); not
  // charged to bus error attribution — same as odometer/vitals above, only
  // the CMD_GET_STATUS read above is the liveness signal.
  refreshUnitExtDiag(f, toI2cAddress(i));
  // Lifetime health refreshes on the same cadence (#406) — a failed homing
  // must not wait for the next probe to surface.
  refreshUnitLifetime(f, toI2cAddress(i));
  // Boot-section verdict on the same cadence (#520).
  refreshUnitBootVerdict(f, i);
  unitFacts[i] = f;
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

#if SERIAL_ENABLE == false
static void rescueTick(int i);  // #498, defined after the twiboot helpers
static void motionBudgetFold(int i);  // #505, defined with the render path
#endif

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
  if (detectedUnitCount == 0) busRecoveryNoteEmptyRow(busRecovery, now);
  followerBusRecoveryTick();
  if (displayWidth <= 0 || detectedUnitCount == 0) return;
  int i = slot;
  slot = heartbeatNextSlot(slot, displayWidth);
  bool drivable = unitDrivable(unitFacts[i]);
  bool ok = busPollHealthOne(i);
  heartbeatApply(unitFacts[i], ok, millis(), HEARTBEAT_MISS_THRESHOLD);
  if (drivable) observeLiveness(i, ok);
  motionBudgetFold(i);  // #505
  rescueTick(i);
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
    if (!unitDrivable(unitFacts[i])) continue;  // #405
    if (checkIfMoving(i) == 1) return true;
  }
  return false;
}

bool busRowMoving() {
#if SERIAL_ENABLE == false
  // A unit in its twiboot window (v1 #88) or a streaming reflash must not be
  // read: report "moving" so the caller waits instead.
  if ((int32_t)(millis() - busProbeInhibitedUntilMs()) < 0) return true;
  if (reflashInProgress(reflashProgress)) return true;
  return isRowMoving();
#else
  return false;
#endif
}

// --- motion admission (#505, MotionBudget.h) ---------------------------------
static MotionRadioGate radioGate;
static MotionBudgetState motionBudget;

// No motion while the radio is in a high-draw phase, bounded.
static void admitMotion() {
  uint32_t holdStart = millis();
  bool logged = false;
  for (;;) {
    motionRadioObserve(radioGate, followerRadioBusy(), millis());
    if (motionRadioQuiet(radioGate, millis())) return;
    if (motionRadioHoldExpired(holdStart, millis())) {
      SerialPrintln(F("motion: radio still busy after the hold cap — moving anyway"));
      return;
    }
    if (!logged) {
      SerialPrintln(F("motion: holding unit moves while the radio is busy"));
      logged = true;
    }
    delay(100);
  }
}

// Blocks until fewer than the budget's cap of tracked units still move.
static void waitForMotionSlot(MotionTracker& movers) {
  while (motionTrackerFull(movers, motionBudget.cap)) {
    for (int k = movers.count - 1; k >= 0; k--) {
      motionTrackerObserve(movers, k, checkIfMoving(movers.unit[k]), millis());
    }
    if (motionTrackerFull(movers, motionBudget.cap)) delay(50);
  }
}

// Folds unit i's since-boot supply minimum into the cap (heartbeat cadence).
static void motionBudgetFold(int i) {
  uint32_t now = millis();
  motionRadioObserve(radioGate, followerRadioBusy(), now);
  const UnitFacts& u = unitFacts[i];
  if (u.vitalsValid &&
      motionBudgetObserveVmin(motionBudget, i, u.vitals.vccMin_mV, now)) {
    SerialPrint(F("motion: rail sag "));
    SerialPrint(u.vitals.vccMin_mV);
    SerialPrint(F(" mV at unit "));
    SerialPrint(toI2cAddress(i));
    SerialPrint(F(" — at most "));
    SerialPrint(motionBudget.cap);
    SerialPrintln(F(" unit(s) move at once"));
  } else if (motionBudgetTick(motionBudget, now)) {
    SerialPrint(F("motion: rail quiet — at most "));
    SerialPrint(motionBudget.cap);
    SerialPrintln(F(" unit(s) move at once"));
  }
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

// A unit clamps the speed byte to SFP_UNIT_SPEED_MAX, so a wider range here
// would silently flatten its top end.
static_assert(MIN_SPEED >= 1 && MAX_SPEED <= SFP_UNIT_SPEED_MAX,
              "the wire speed range must stay inside what a unit accepts");

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
  lastFrame = segment;
  lastFrameSpeed = webSpeed;
  lastFrameValid = true;

  waitForRowToStop();
  admitMotion();

  MotionTracker movers;
  int commanded[UNITS_AMOUNT];
  for (int i = 0; i < UNITS_AMOUNT; i++) commanded[i] = -1;

  int commandedCount = 0;
  for (int i = 0; i < width; i++) {
    if (!unitDrivable(unitFacts[i])) continue;  // #405
    int letter = translateLetterToInt(frame[i]);
    if (letter < 0) continue;  // char not on the drum: leave the unit be
    // #505: at most the budget's cap moving at once — the steady draw of
    // every energised stepper, not just the start spike, sags the rail.
    waitForMotionSlot(movers);
    // #324: spread the flap inrush — pause before opening each new group so a
    // full row's steppers don't spin up at once and brown out the rail.
    if (renderStaggerShouldSettle(commandedCount, RENDER_STAGGER_BATCH)) {
      delay(RENDER_STAGGER_SETTLE_MS);
    }
    writeToUnit(i, letter, speed);
    motionTrackerAdd(movers, i, millis());
    commanded[i] = letter;
    commandedCount++;
  }

  waitForRowToStop();

  // Closed-loop verification (v1 #106): read back, re-send once on mismatch.
  int resent = 0;
  MotionTracker resendMovers;
  for (int i = 0; i < UNITS_AMOUNT; i++) {
    if (commanded[i] < 0 || !unitDrivable(unitFacts[i])) continue;  // #405
    int shown;
    if (!readUnitDisplayedLetter(toI2cAddress(i), shown)) continue;
    if (shown == commanded[i]) continue;
    waitForMotionSlot(resendMovers);  // resends are moves too (#505)
    writeToUnit(i, commanded[i], speed);
    motionTrackerAdd(resendMovers, i, millis());
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

// Value + bitwise complement, then read it back (#405) — was fire-and-forget.
int busWriteOffset(uint8_t i2cAddress, int16_t value) {
  uint8_t enc[SET_OFFSET_PAYLOAD_LEN];
  setOffsetEncode(value, enc);
  Wire.beginTransmission(i2cAddress);
  Wire.write((uint8_t)SFP_CMD_SET_OFFSET);
  Wire.write(enc, SET_OFFSET_PAYLOAD_LEN);
  int txStatus = Wire.endTransmission();
  if (txStatus != 0) return txStatus;
  // The unit persists in loop context, not its TWI ISR — let the write drain.
  delay(UNIT_OFFSET_WRITE_SETTLE_MS);
  int16_t readBack = 0;
  if (!readUnitOffset(i2cAddress, readBack)) return UNIT_BUS_OFFSET_UNVERIFIED;
  if (readBack != value) return UNIT_BUS_OFFSET_MISMATCH;
  return 0;
}

int busJog(uint8_t i2cAddress, int steps) {
  admitMotion();
  Wire.beginTransmission(i2cAddress);
  Wire.write((uint8_t)SFP_CMD_JOG);
  uint8_t jog[JOG_PAYLOAD_LEN];
  jogEncode(maintEncodeJogByte(steps), jog);
  Wire.write(jog, JOG_PAYLOAD_LEN);
  return Wire.endTransmission();
}

int busHome(uint8_t i2cAddress) {
  admitMotion();
  Wire.beginTransmission(i2cAddress);
  writeGuardedOpcode(SFP_CMD_HOME);
  return Wire.endTransmission();
}

int busIdentify(uint8_t i2cAddress) {
  Wire.beginTransmission(i2cAddress);
  writeGuardedOpcode(SFP_CMD_IDENTIFY);
  return Wire.endTransmission();
}

int busResetOdometer(uint8_t i2cAddress) {
  Wire.beginTransmission(i2cAddress);
  writeGuardedOpcode(SFP_CMD_RESET_ODOMETER);
  return Wire.endTransmission();
}

// Feature gates (#409): complement-protected write, then a GET_LIFETIME
// read-back — the same verified shape as busWriteOffset above, and the
// mechanism that lets this row's units have a motion gate flipped without
// pulling five Nanos for a reflash.
int busSetGates(uint8_t i2cAddress, uint8_t gates) {
  uint8_t enc[SET_GATES_PAYLOAD_LEN];
  setGatesEncode(gates, enc);
  Wire.beginTransmission(i2cAddress);
  Wire.write((uint8_t)SFP_CMD_SET_GATES);
  Wire.write(enc, SET_GATES_PAYLOAD_LEN);
  int txStatus = Wire.endTransmission();
  if (txStatus != 0) return txStatus;
  // The unit persists in loop context, not its TWI ISR — let the write drain.
  delay(UNIT_GATES_WRITE_SETTLE_MS);
  UnitLifetimeFacts lt;
  if (!readUnitLifetime(i2cAddress, lt)) return UNIT_BUS_GATES_UNVERIFIED;
  if (lt.featureGates != gates) return UNIT_BUS_GATES_MISMATCH;
  return 0;
}

int busRebootToBootloader(uint8_t i2cAddress) {
  Wire.beginTransmission(i2cAddress);
  // Bare on purpose: the one-byte form is the fixed-forever one every unit
  // accepts (#512), and a unit already sitting in twiboot ACKs exactly one
  // byte — a guard byte would turn that status into a NACK.
  Wire.write((uint8_t)SFP_CMD_ENTER_BOOTLOADER);
  return Wire.endTransmission();
}

// Soft watchdog reset — stays in sketch mode (v1 #47/#113).
static int rebootUnit(uint8_t i2cAddress) {
  Wire.beginTransmission(i2cAddress);
  writeGuardedOpcode(SFP_CMD_REBOOT);
  return Wire.endTransmission();
}

int busStartSelfTest(uint8_t i2cAddress) {
  admitMotion();
  Wire.beginTransmission(i2cAddress);
  writeGuardedOpcode(SFP_CMD_START_SELF_TEST);
  return Wire.endTransmission();
}

bool busReadSelfTest(uint8_t i2cAddress, UnitSelfTestReading& out) {
  uint8_t buf[SELFTEST_REPLY_LEN];
  if (!queryUnit(i2cAddress, (uint8_t)SFP_CMD_GET_SELF_TEST, buf,
                 SELFTEST_REPLY_LEN)) {
    return false;
  }
  return selfTestReadbackValid(buf, out);
}

bool busReadBootInfo(uint8_t i2cAddress, BootUpdateReport& out) {
  uint8_t buf[BOOT_INFO_REPLY_LEN];
  if (!queryUnit(i2cAddress, (uint8_t)SFP_CMD_GET_BOOT_INFO, buf,
                 BOOT_INFO_REPLY_LEN)) {
    return false;
  }
  return bootInfoDecode(buf, out);
}

int busBootUpdate(uint8_t i2cAddress, uint8_t stage) {
  Wire.beginTransmission(i2cAddress);
  Wire.write((uint8_t)SFP_CMD_BOOT_UPDATE);
  Wire.write(stage);
  Wire.write((uint8_t)~stage);
  return Wire.endTransmission();
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

#if SERIAL_ENABLE == false
// Runtime rescue of lost units (#498, UnitRescuePolicy.h). Reached only from
// the heartbeat tick: never inside the probe-inhibit window, never during a
// reflash. A dead bus is the row-wide recovery's job, not a per-unit probe's.
static UnitRescueState rescueStates[UNITS_AMOUNT];

static void rescueTick(int i) {
  UnitRescueState& rs = rescueStates[i];
  unitFacts[i].rescueExits = rs.exits;  // a probe rebuilds unitFacts
  if (unitRescueObserve(rs, unitFacts[i])) {
    SerialPrint(F("unit "));
    SerialPrint(toI2cAddress(i));
    SerialPrintln(F(": answering again — re-showing the last frame"));
    reshowPending = lastFrameValid;  // run by the next recovery tick
    return;
  }
  if (busRecovery.dead) return;
  if (!unitRescueDue(unitFacts[i], rs, millis())) return;
  uint8_t addr = (uint8_t)toI2cAddress(i);
  UnitRescueProbe probe = UnitRescueProbe::NoAck;
  Wire.beginTransmission(addr);
  if (Wire.endTransmission() == 0) {
    probe = UnitRescueProbe::SketchSilent;
    if (isUnitInBootloader(addr)) {
      twibootAddr = addr;
      twibootExit();
      probe = UnitRescueProbe::Bootloader;
    }
  }
  unitRescueNoteAttempt(rs, millis(), probe);
  unitFacts[i].rescueExits = rs.exits;
  SerialPrint(F("unit "));
  SerialPrint(addr);
  if (probe == UnitRescueProbe::Bootloader) {
    SerialPrint(F(": lost — found in twiboot, started its app (rescue #"));
    SerialPrint(rs.exits);
    busArmProbeInhibit(millis() + 3000);  // let the sketch boot
  } else if (probe == UnitRescueProbe::NoAck) {
    SerialPrint(F(": lost — no ACK (attempt "));
    SerialPrint(rs.attempts);
  } else {
    SerialPrint(F(": lost — ACKs but status reads fail (attempt "));
    SerialPrint(rs.attempts);
  }
  SerialPrintln(F(")"));
}
#endif

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
// True when every unit reported idle inside the timeout.
static bool waitForBatchIdle(const uint8_t* addrs, int count,
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
    if (allIdle) return true;
    delay(100);
  }
  return false;
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
// bundle, page-verified — don't re-read over I2C and risk pinning twiboot),
// then re-probes the row if anything was flashed.
//
// The re-probe is not optional bookkeeping (#462). Every caller reaches here
// through a probe taken with the targets sitting in twiboot, where the offset
// / odometer / lifetime reads cannot succeed — so those facts come back
// INVALID and nothing else repopulates them: busPollHealth is a health poll,
// not a probe, and it never reads the offset. GET /unit/offset then served
// "no valid offset" for a whole freshly-flashed row whose calibration was
// perfectly intact, at the exact moment an operator checks it, and
// restore-unit-offsets.sh read the same cache and called the row UNREADABLE.
// The invalidation is right (UnitHealth.h's documented lifecycle: reads drop
// when a bootloader reboot invalidates them) — the repopulation was missing.
// This is what the S3 already does at the end of runReflashJob.
static void flashBootloaderUnits(uint8_t onlyAddr = 0) {
  uint8_t batch[REFLASH_BATCH_SIZE];
  int batchCount = 0;
  int flashed = 0;
  for (int i = 0; i < UNITS_AMOUNT; i++) {
    uint8_t addr = (uint8_t)toI2cAddress(i);
    // A targeted run leaves every other bootloader-mode unit alone (#513).
    if (!reflashShouldFlashUnit(unitFacts[i], addr, onlyAddr)) continue;
    reflashProgressUnitStart(reflashProgress, addr);
    bool ok = flashUnitFromProgmem(addr);
    reflashProgressUnitResult(reflashProgress, ok);
    if (ok) {
      unitFacts[i].state = 1;
      strncpy(unitFacts[i].version, BUNDLED_UNIT_REV, 8);
      unitFacts[i].version[8] = '\0';
      unitFacts[i].fwStatus = 0;
      batch[batchCount++] = addr;
      flashed++;
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
  // Runs only when something was actually flashed — a no-op sweep leaves the
  // facts it was handed alone.
  //
  // Position is load-bearing: it must follow the trailing batch settle. This
  // probe sends isUnitInBootloader()'s CMD_ACCESS_MEMORY, which zeroes
  // twiboot's boot_timeout and pins it alive on a unit still inside its
  // post-reset window (v1 #88 — the quirk main.cpp's 1500 ms pre-probe delay
  // exists for). The settle closes that window by construction, not by an
  // assumed margin:
  //
  //   - only a WRITE of CMD_WAIT/CMD_ACCESS_MEMORY/CMD_SWITCH_APPLICATION
  //     pins twiboot (UnitBootloader/main.c TWI_data_write). A bare read
  //     cannot, and while cmd == CMD_WAIT a read returns 0xFF from
  //     TWI_data_read's default branch — never 0x00.
  //   - checkIfMoving returns that 0xFF verbatim (and -1 for no answer), and
  //     waitForBatchIdle treats anything != 0 as not-idle. So the settle
  //     CANNOT return while a batch member is in the bootloader; only a unit
  //     running its sketch reports 0.
  //   - twiboot's TIMEOUT_MS is 1000 and the Nano's SFP_CMD_REBOOT path adds
  //     delay(10) + WDTO_15MS, so a unit jumps to its app at about T+1025 ms.
  //     The settle's delay(1000) puts the first poll at T+1000 — still inside
  //     the window, so it reads non-zero and the 100 ms loop cannot exit
  //     before T+1100, after the unit has already left. Independent of
  //     REFLASH_BATCH_SIZE.
  //
  // Units OUTSIDE the batch — the ones that failed to flash — are still in
  // twiboot and this probe does pin them there. That is the intended end
  // state: they show as bootloader and the next sweep flashes them.
  //
  // Same position and same preceding settle as the S3's runReflashJob; keep
  // the two flows in step rather than tuning one of them alone.
  if (flashed > 0) busProbe();
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

uint8_t busLastReflashFailed() { return reflashProgress.failed; }

void busRunReflashJob(uint8_t onlyAddr) {
#if SERIAL_ENABLE == false
  SerialPrintln(F("Unit reflash starting (throttled)..."));
  uint8_t targets[UNITS_AMOUNT];
  int rebooted = reflashCollectRebootTargets(unitFacts, UNITS_AMOUNT,
                                             SFP_I2C_ADDRESS_BASE, targets);
  rebooted = reflashFilterToAddress(targets, rebooted, onlyAddr);
  for (int k = 0; k < rebooted; k++) busRebootToBootloader(targets[k]);
  if (rebooted > 0) delay(TWIBOOT_STARTUP_MS);
  busProbe();  // reflash-internal probe (#205 exception to the inhibit)
  uint8_t flashTargets[UNITS_AMOUNT];
  int n = reflashCollectFlashTargets(unitFacts, UNITS_AMOUNT,
                                     SFP_I2C_ADDRESS_BASE, flashTargets);
  n = reflashFilterToAddress(flashTargets, n, onlyAddr);
  reflashProgressBegin(reflashProgress, n);
  flashBootloaderUnits(onlyAddr);
  reflashProgressFinish(reflashProgress, false);
  busPollHealth();
  // Staggered boot-home of the just-flashed units (#309): a reflashed unit
  // reboots UNHOMED, so without this the next cluster render would home the
  // whole row at once — the #305 inrush class. Targets only unhomed units.
  followerBootHome();
  SerialPrintln(F("Unit reflash complete."));
#else
  (void)onlyAddr;
#endif
}

// The start probes below run inside the probe-inhibit window this op arms.
// That is safe only because a report request is not one of the first bytes the
// bootloader pins itself on (0x00..0x02): it answers it by leaving for the
// sketch.
static_assert(SFP_CMD_GET_BOOT_INFO > 0x02,
              "GET_BOOT_INFO would pin twiboot — the stage 1 start probes "
              "would then hold a unit in its bootloader");

void busRunBootUpdate(uint32_t seq, uint8_t addr, MaintResult& result) {
#if SERIAL_ENABLE == false
  BootUpdateReport info;
  if (!busReadBootInfo(addr, info)) {
    result = {seq, MaintOutcome::PostconditionFail,
              MaintReason::BootInfoReadFail};
    return;
  }
  BootUpdatePlan plan = bootUpdateDecide(info);
  if (plan.terminal != BOOT_PLAN_PROCEED) {
    MaintOutcome o = MaintOutcome::PostconditionFail;
    MaintReason r = MaintReason::BootStateUnknown;
    switch (plan.terminal) {
      case BOOT_PLAN_ALREADY_NEW:
        o = MaintOutcome::Ok; r = MaintReason::BootAlreadyNew; break;
      case BOOT_PLAN_LOCK_REFUSED:
        r = MaintReason::BootLockRefused; break;
      case BOOT_PLAN_UNKNOWN_STATE:
        r = MaintReason::BootStateUnknown; break;
      default: break;
    }
    result = {seq, o, r};
    return;
  }
  // A request sent mid-move is held by the unit until the move ends and would
  // then run behind this op's back, so the drum settles first — and a drum
  // that does not settle ends the op here (#516).
  if (!waitForBatchIdle(&addr, 1, 8000)) {
    result = {seq, MaintOutcome::PostconditionFail, MaintReason::BootUnitBusy};
    return;
  }
  if (plan.needStage1) {
    if (busBootUpdate(addr, 1) != 0) {
      result = {seq, MaintOutcome::WireFail, MaintReason::None};
      return;
    }
    busArmProbeInhibit(millis() + 3000);
    // A unit that never left the bus did not start: report what it said.
    bool started = bootStage1WentOffBus(
        [&]() {
          BootUpdateReport still;
          if (!busReadBootInfo(addr, still)) return false;
          info = still;
          return true;
        },
        [](uint16_t ms) { delay(ms); });
    if (!started) {
      result = {seq, MaintOutcome::PostconditionFail,
                maintReasonForBootFailure(bootResultFailure(info.lastResult),
                                          MaintReason::BootNotStarted)};
      return;
    }
    waitForBatchIdle(&addr, 1, 10000);
    busHome(addr);
    waitForBatchIdle(&addr, 1, 20000);
    if (!busReadBootInfo(addr, info)) {
      result = {seq, MaintOutcome::PostconditionFail,
                MaintReason::BootUnitLost};
      return;
    }
    if (info.state != BOOT_STATE_PAGE7_INSTALLED) {
      result = {seq, MaintOutcome::PostconditionFail,
                MaintReason::BootVerifyFailed};
      return;
    }
  }
  if (plan.needStage2) {
    if (!plan.needStage1) {
      // Resuming a unit that already carries page 7: nothing above homed it,
      // and an unhomed unit refuses the stage.
      busHome(addr);
      waitForBatchIdle(&addr, 1, 20000);
      if (!busReadBootInfo(addr, info)) {
        result = {seq, MaintOutcome::PostconditionFail,
                  MaintReason::BootUnitLost};
        return;
      }
    }
    const uint8_t resultBeforeSend = info.lastResult;
    if (busBootUpdate(addr, 2) != 0) {
      result = {seq, MaintOutcome::WireFail, MaintReason::None};
      return;
    }
    delay(300);
    BootPollVerdict verdict = BOOT_POLL_WAIT;
    bool anyRead = false;
    uint32_t start = millis();
    while (millis() - start < 5000) {
      if (busReadBootInfo(addr, info)) {
        anyRead = true;
        verdict = bootStage2Poll(info, resultBeforeSend);
        if (verdict != BOOT_POLL_WAIT) break;
      }
      delay(100);
    }
    if (verdict != BOOT_POLL_DONE) {
      // No report at all is a lost unit, not a failed verify; otherwise the
      // unit's own result names the cause.
      result = {seq, MaintOutcome::PostconditionFail,
                anyRead ? maintReasonForBootFailure(
                              bootResultFailure(info.lastResult),
                              MaintReason::BootVerifyFailed)
                        : MaintReason::BootUnitLost};
      return;
    }
  }
  busArmProbeInhibit(millis() + 3000);
  result = {seq, MaintOutcome::Ok, MaintReason::None};
#else
  (void)seq; (void)addr; (void)result;
#endif
}

void busRunBootDump(uint32_t seq, uint8_t addr,
                    BootDumpSlot& slot, uint8_t* outBytes) {
#if SERIAL_ENABLE == false
  slot.seq = seq;
  slot.addr = addr;
  if (busRebootToBootloader(addr) != 0) {
    slot.outcome = BootDumpOutcome::EnterFail;
    busArmProbeInhibit(millis() + 3000);
    return;
  }
  delay(TWIBOOT_STARTUP_MS);

  twibootAddr = addr;
  bool bootloaderLive = false;
  for (int attempt = 0; attempt < 5; attempt++) {
    if (twibootPing() == 0) { bootloaderLive = true; break; }
    delay(100);
  }
  if (!bootloaderLive) {
    slot.outcome = BootDumpOutcome::BootloaderSilent;
  } else if (!twibootVerifyChip()) {
    slot.outcome = BootDumpOutcome::ChipMismatch;
  } else {
    slot.outcome = BootDumpOutcome::Ok;
    for (int page = 0; page < BOOT_SECTION_LEN / TWIBOOT_PAGE_SIZE; page++) {
      uint16_t flashAddr =
          (uint16_t)(BOOT_SECTION_START + page * TWIBOOT_PAGE_SIZE);
      uint8_t* dst = outBytes + page * TWIBOOT_PAGE_SIZE;
      if (!twibootReadFlashPage(flashAddr, dst) &&
          !twibootReadFlashPage(flashAddr, dst)) {
        slot.outcome = BootDumpOutcome::ReadFail;
        break;
      }
    }
  }

  bool exited = false;
  for (int attempt = 0; attempt < 3 && !exited; attempt++) {
    exited = twibootExit() == 0;
    if (!exited) delay(20);
  }
  if (!exited) {
    char addrHex[8];
    snprintf(addrHex, sizeof(addrHex), "0x%02x", addr);
    SerialPrint(F("Unit "));
    SerialPrint(addrHex);
    SerialPrintln(F(": twiboot exit failed after boot-section read"));
  } else {
    delay(2000);
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) rebootUnit(addr);
  }

  waitForBatchIdle(&addr, 1, 10000);
  if (busHome(addr) == 0) waitForBatchIdle(&addr, 1, 20000);
  reshowPending = lastFrameValid;
  busArmProbeInhibit(millis() + 3000);

  if (slot.outcome == BootDumpOutcome::Ok) {
    slot.crc32 = bootDumpCrc32(outBytes, BOOT_SECTION_LEN);
  }
  {
    char logBuf[72];
    snprintf(logBuf, sizeof(logBuf), "boot-dump unit 0x%02x -> %s (crc32 %08lx)",
             addr, bootDumpOutcomeName(slot.outcome),
             (unsigned long)slot.crc32);
    SerialPrintln(logBuf);
  }
#else
  (void)seq; (void)addr; (void)slot; (void)outBytes;
#endif
}
