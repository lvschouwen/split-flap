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
#include "FlapLetters.h"  // shared character/speed mapping
#include "FollowerConfig.h"
#include "FollowerWifi.h"  // followerRadioBusy (#505)
#include "HeartbeatPolicy.h"  // pure heartbeat miss/schedule logic (#310)
#include "MotionBudget.h"  // motion admission (#505)
#include "RenderStagger.h"  // sub-frame inrush stagger (#324)
#include "SplitFlapProtocol.h"
#include "TwibootFlash.h"  // the twiboot client; this file is its Wire adapter
#include "UnitBusCore.h"   // the unit sketch protocol, on the same adapter
#include "UnitBusTwiboot.h"  // rescue probe, boot-section read, unit flash
#include "UnitAssets.h"  // UNIT_FIRMWARE_BIN (build_assets.py)
#include "UnitProtocolHelpers.h"
#include "UnitTimings.h"
#include "UnitRescuePolicy.h"  // runtime rescue of lost units (#498)
#include "BootUpdateOp.h"     // the shared in-system twiboot update (#499)
#include "UnitUpdateJob.h"    // quiet wait + boot sweep around the flash loop
#include "FollowerScanLog.h"  // which units a scan logs
#include "BootDumpOp.h"       // the shared boot-section dump (#511)

UnitFacts unitFacts[UNITS_AMOUNT];
int displayWidth = UNITS_AMOUNT;
int detectedUnitCount = 0;
ReflashProgress reflashProgress;

// v1 #88: probes must never land inside a twiboot window.
static uint32_t probeInhibitUntilMs = 0;

uint32_t busProbeInhibitedUntilMs() { return probeInhibitUntilMs; }
void busArmProbeInhibit(uint32_t untilMs) { probeInhibitUntilMs = untilMs; }

// Freshness bookkeeping (miss counter / stale latch / lastSeenMs) is the pure
// heartbeatApply() in HeartbeatPolicy.h — same as the Master, copy policy.

// How long a segment write waits for the row to stop before assuming a
// unit is physically stuck (v1 value).

static int toI2cAddress(int unitIndex) {
  return SFP_I2C_ADDRESS_BASE + unitIndex;
}

void busInit() {
#if SERIAL_ENABLE == false
  // ESP-01: SDA=GPIO1(TX), SCL=GPIO3(RX) — v1 hardware truth; the Wire
  // buffer is bumped to 256 via -DI2C_BUFFER_LENGTH for twiboot's
  // 132-byte page writes.
  Wire.begin(1, 3);
  // #488: a warm restart (new firmware, the master's Restart) leaves the
  // Nanos powered, so one held SDA mid-byte survives it and the boot probe
  // finds nothing. Clock it free before anything scans.
  uint8_t status = Wire.status();
  if (status != 0) {
    SerialPrint(F("bus: line held at boot, state "));
    SerialPrintln(status);
  }
#endif
}

// Bus health counters (#306), sent to the master in Status so a flaky row
// shows. Same scope as the S3's i2cTx/i2cErr: every
// sketch-protocol write transaction (frames, queries, maintenance ops) counts
// as tx; err counts failed writes AND failed read legs. Not counted: the
// ~10 Hz rotation polls, the bus-scan probe and the twiboot page stream.
static uint32_t busTxCount = 0;
static uint32_t busErrCount = 0;
uint32_t followerBusTxCount() { return busTxCount; }
uint32_t followerBusErrCount() { return busErrCount; }

// Per-unit error attribution (#367), as on the S3.
static UnitErrorLedger<UNITS_AMOUNT> unitErrors;

// The unit protocol is shared/UnitBusCore.h and the twiboot protocol
// shared/TwibootFlash.h; this is the Wire adapter both run on. No bus rebuild
// after a short read here: that is an ESP32 I2C driver quirk (#207).
namespace {
struct WireTwibootBus {
  void beginTransmission(uint8_t addr) { Wire.beginTransmission(addr); }
  int endTransmission(bool stop) { return Wire.endTransmission(stop); }
  size_t write(uint8_t b) { return Wire.write(b); }
  uint8_t requestFrom(uint8_t addr, uint8_t qty) {
    return Wire.requestFrom(addr, qty);
  }
  int read() { return Wire.read(); }
  int available() { return Wire.available(); }
  uint32_t nowMs() { return millis(); }
  void sleepMs(uint32_t ms) { delay(ms); }
  void readFailed() {}
  int endCounted() {
    int status = Wire.endTransmission();
    busTxCount++;
    if (status != 0) busErrCount++;
    return status;
  }
  void noteReadError() { busErrCount++; }
  void mark(UnitBusAct, uint8_t) {}
};
WireTwibootBus unitBus;

// What the shared probe/poll code found worth a log line; the text stays in
// flash on this chip.
struct UnitBusNotes {
  void identityRead(uint8_t, const UnitFacts&, bool) {}
  void driftSeen(uint8_t i2cAddress, const DriftLogDecision& drift,
                 const UnitDiagReading&) {
    SerialPrint(F("Unit "));
    SerialPrint(i2cAddress);
    SerialPrint(F(" drifted: "));
    SerialPrint(drift.newEvents);
    SerialPrintln(F(" new event(s) — unit auto re-homing"));
  }
  void bootVerdictChanged(uint8_t i2cAddress, const UnitFacts& fact,
                          const BootUpdateReport& r) {
    char logBuf[88];
    snprintf(logBuf, sizeof(logBuf),
             "Unit 0x%02x bootloader %s - crc32 %08lx (expected %08lx)",
             i2cAddress, bootIntegrityName(fact.bootVerdict),
             (unsigned long)r.bootCrc32, (unsigned long)BOOT_CURRENT_CRC32);
    SerialPrintln(logBuf);
  }
};
UnitBusNotes unitBusNotes;
}  // namespace

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
// instead of evicting the ring (the #436 flood lesson).
#define BUS_RECOVERY_LOGGED_ATTEMPTS 3

const BusRecoveryState& followerBusRecovery() { return busRecovery; }

#if SERIAL_ENABLE == false
// Only drivable units feed the detector: busPollHealthOne() returns false for
// the rest without touching the bus, which is no evidence either way. The
// reshow is staged, not run here: a render blocks up to UNIT_SHOW_STUCK_TIMEOUT_MS.
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

// Per-unit memory of the last boot verdict logged (#520): outside the facts,
// so a probe rescan (which rebuilds them) does not repeat a finding.
static uint8_t bootVerdictLogged[UNITS_AMOUNT];

static bool isUnitInBootloader(int i2cAddress) {
  return twibootIsBootloader(unitBus, (uint8_t)i2cAddress);
}

void busProbe() { busProbeQuiet(false); }

void busProbeQuiet(bool quiet) {
#if SERIAL_ENABLE == false
  if (!quiet) SerialPrintln(F("Scanning I2C bus for units..."));
  // #468: the I2C reads below yield, and a web handler (GET /settings reads
  // the row width) or the link can run in between — zeroing a slot up front
  // and refilling it field by field would let a reader landing mid-probe see
  // the unit as absent/unversioned. Each slot is
  // therefore probed into a scratch struct and published with one
  // assignment (no yield inside a struct copy on this single-core part),
  // and the count is published once at the end.
  int detected = 0;
  int states[UNITS_AMOUNT];
  uint8_t statesBefore[UNITS_AMOUNT];
  for (int i = 0; i < UNITS_AMOUNT; i++) {
    UnitFacts f{};
    states[i] = 0;
    statesBefore[i] = unitFacts[i].state;
    int i2cAddress = toI2cAddress(i);
    Wire.beginTransmission(i2cAddress);
    if (Wire.endTransmission() != 0) {
      unitErrors.fold(f, i);  // the tally outlives a rescan (#367)
      unitFacts[i] = f;
      continue;
    }

    bool inBootloader = isUnitInBootloader(i2cAddress);
    f.state = inBootloader ? 2 : 1;
    states[i] = f.state;
    detected++;
    if (inBootloader) {
      // Which bootloader, and its lock and fuse bytes where it serves them
      // (#541/#543). The chipinfo probe above already holds its countdown.
      twibootReadIdentity(unitBus, (uint8_t)i2cAddress, f.bootloader);
      unitErrors.fold(f, i);  // the tally outlives a rescan (#367)
      unitFacts[i] = f;
      continue;
    }

    unitProbeSketchUnit(unitBus, unitBusNotes, f, (uint8_t)i2cAddress,
                        BUNDLED_UNIT_REV, BUNDLED_UNIT_REV_EQUIV,
                        bootVerdictLogged[i]);
    unitErrors.fold(f, i);
    unitFacts[i] = f;
  }
  detectedUnitCount = detected;
  displayWidth = computeDisplayWidth(states, UNITS_AMOUNT);
  if (quiet) return;
  // One line per unit per change of finding: a row that stays outdated says
  // so once, not on every rescan (the ring is small).
  static uint8_t scanLogged[UNITS_AMOUNT];
  for (int i = 0; i < UNITS_AMOUNT; i++) {
    char line[FOLLOWER_SCAN_LINE_CAP];
    uint8_t finding = followerScanLine(line, sizeof(line),
                                       (uint8_t)toI2cAddress(i), unitFacts[i],
                                       statesBefore[i]);
    if (finding != SCAN_FINDING_NONE && finding != scanLogged[i]) {
      SerialPrintln(line);
    }
    scanLogged[i] = finding;
  }
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
  // Every column's tally, before the state gate: a render-time write failure
  // must show within one heartbeat tick, whichever slot this poll lands on.
  unitErrors.fold(unitFacts, UNITS_AMOUNT);
  if (!unitDrivable(unitFacts[i])) {  // #405
    unitFacts[i].statusValid = false;
    unitFacts[i].bootVerdict = BOOT_INTEGRITY_UNREAD;
    return false;
  }
  // #468: same publish-on-complete rule as busProbe() above — the reads
  // below yield, so mutate a scratch copy (seeded from the published slot:
  // a poll updates, it never resets) and publish with one assignment.
  UnitFacts f = unitFacts[i];
  bool ok = unitPollHealth(unitBus, unitBusNotes, f, (uint8_t)toI2cAddress(i),
                           resetBaselines[i], bootVerdictLogged[i]);
  // Only the status read is the liveness signal, so only its failure is
  // charged to the unit (#367).
  if (!ok) unitErrors.note(i, millis());
  unitErrors.fold(f, i);
  unitFacts[i] = f;
  return ok;  // CMD_GET_STATUS liveness signal for the heartbeat (#310)
#else
  (void)i;
  return false;
#endif
}

#if SERIAL_ENABLE == false
static void pollHealthAll() {
  uint32_t now = millis();
  for (int i = 0; i < UNITS_AMOUNT; i++) {
    bool ok = busPollHealthOne(i);
    heartbeatApply(unitFacts[i], ok, now, HEARTBEAT_MISS_THRESHOLD);
  }
}

// The update job's own poll. The job holds the gate, so the public poll
// stands down for it; this one waits out the twiboot window first — a status
// read inside it can pin a unit in its bootloader.
static void jobPollHealth() {
  int32_t remaining = (int32_t)(busProbeInhibitedUntilMs() - millis());
  if (remaining > 0) delay((uint32_t)remaining);
  pollHealthAll();
}
#endif

void busPollHealth() {
#if SERIAL_ENABLE == false
  if (reflashInProgress(reflashProgress)) return;
  pollHealthAll();
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

// A render's write; a failure is charged to its unit (#367).
static int writeToUnit(int unitIndex, int letter, int speed) {
  int status = unitWriteLetter(unitBus, (uint8_t)toI2cAddress(unitIndex),
                               (uint8_t)letter, (uint8_t)speed);
  if (status != 0) unitErrors.note(unitIndex, millis());
  return status;
}

// 0 idle, 1 rotating, -1 offline.
static int checkIfMoving(int unitIndex) {
  return unitMovingStatus(unitBus, (uint8_t)toI2cAddress(unitIndex));
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
    if (millis() - waitStart > UNIT_SHOW_STUCK_TIMEOUT_MS) {
      SerialPrintln(F("Row-stop wait timed out — a unit may be stuck"));
      break;
    }
    delay(100);
  }
}

void busShowSegment(const String& segment, int webSpeed) {
#if SERIAL_ENABLE == false
  const int width = displayWidth;
  const int speed = convertSpeedToUnit(webSpeed);
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
    int letter = flapLetterOrBlank(frame[i]);
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
    if (!unitReadDisplayedLetter(unitBus, (uint8_t)toI2cAddress(i), shown)) {
      continue;
    }
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

// The frames, verifies and settle times are shared/UnitBusCore.h; an op that
// starts motion first waits for a quiet radio (#505).
int busWriteOffset(uint8_t i2cAddress, int16_t value) {
  return unitWriteOffset(unitBus, i2cAddress, value);
}

int busJog(uint8_t i2cAddress, int steps) {
  admitMotion();
  return unitJog(unitBus, i2cAddress, steps);
}

int busHome(uint8_t i2cAddress) {
  admitMotion();
  return unitHome(unitBus, i2cAddress);
}

int busIdentify(uint8_t i2cAddress) { return unitIdentify(unitBus, i2cAddress); }

int busSetAddress(uint8_t i2cAddress, uint8_t newAddress) {
  return unitSetAddress(unitBus, i2cAddress, newAddress);
}

int busClearAddress(uint8_t i2cAddress) { return unitClearAddress(unitBus, i2cAddress); }

// The speed a row shows at before the master has named one.
void busReshowLastFrame() { reshowPending = lastFrameValid; }

#define HOME_ALL_SPEED 80

void busHomeAll() {
  const bool hadText = lastFrameValid;
  const String text = lastFrame;
  // A row that has shown nothing yet has no speed of its own.
  const int speed = hadText ? lastFrameSpeed : HOME_ALL_SPEED;
  String row;
  for (int i = 0; i < displayWidth; i++) row += '-';
  busShowSegment(row, speed);
  delay(2000);
  for (int i = 0; i < displayWidth; i++) row[i] = '.';
  busShowSegment(row, speed);
  if (hadText) {
    busShowSegment(text, speed);
  } else {
    lastFrameValid = false;  // the dots are not this row's text
  }
}

int busResetOdometer(uint8_t i2cAddress) {
  return unitResetOdometer(unitBus, i2cAddress);
}

int busSetGates(uint8_t i2cAddress, uint8_t gates) {
  return unitSetGates(unitBus, i2cAddress, gates);
}

int busRebootToBootloader(uint8_t i2cAddress) {
  return unitEnterBootloader(unitBus, i2cAddress);
}

int busStartSelfTest(uint8_t i2cAddress) {
  admitMotion();
  return unitStartSelfTest(unitBus, i2cAddress);
}

bool busReadSelfTest(uint8_t i2cAddress, UnitSelfTestReading& out) {
  return unitReadSelfTest(unitBus, i2cAddress, out);
}

bool busReadBootInfo(uint8_t i2cAddress, BootUpdateReport& out) {
  return unitReadBootInfo(unitBus, i2cAddress, out);
}

int busBootUpdate(uint8_t i2cAddress, uint8_t stage) {
  return unitSendBootUpdate(unitBus, i2cAddress, stage);
}

// --- twiboot flash (v1 ServiceFirmwareFunctions port) --------------------------------

static String flashError;

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
  if (unitHeldRecheckDue(unitFacts[i], rs, millis())) {
    rs.lastAttemptMs = millis();
    if (!isUnitInBootloader(toI2cAddress(i))) {
      // Power-cycled or replaced: a rescan finds out what is there now.
      SerialPrint(F("unit "));
      SerialPrint(toI2cAddress(i));
      SerialPrintln(F(": no longer held in its bootloader — rescanning"));
      busProbe();
      busPollHealth();
    }
    return;
  }
  if (!unitRescueDue(unitFacts[i], rs, millis())) return;
  uint8_t addr = (uint8_t)toI2cAddress(i);
  TwibootIdentity bootloader;
  UnitRescueProbe probe = unitRescueProbe(unitBus, addr, bootloader);
  unitRescueNoteAttempt(rs, millis(), probe);
  unitFacts[i].rescueExits = rs.exits;
  SerialPrint(F("unit "));
  SerialPrint(addr);
  if (probe == UnitRescueProbe::Bootloader) {
    SerialPrint(F(": lost — found in twiboot, started its app (rescue #"));
    SerialPrint(rs.exits);
    busArmProbeInhibit(millis() + UNIT_PROBE_INHIBIT_MS);  // let the sketch boot
  } else if (probe == UnitRescueProbe::NoAck) {
    SerialPrint(F(": lost — no ACK (attempt "));
    SerialPrint(rs.attempts);
  } else if (probe == UnitRescueProbe::CrashHeld) {
    // A bootloader unit from here on: no longer "lost", so no further rescue
    // probes, and the update job flashes it where it sits.
    SerialPrint(F(": lost — held in its bootloader after crash resets; left "
                  "there as a reflash target (count "));
    SerialPrint(bootloader.crashCount);
    unitFactsBecomeBootloader(unitFacts[i], bootloader);
  } else {
    SerialPrint(F(": lost — ACKs but status reads fail (attempt "));
    SerialPrint(rs.attempts);
  }
  SerialPrintln(F(")"));
}
#endif

// Flash-stored text for a failed step: string literals live in RAM on this
// chip, so the shared twibootStepName() table is not used here.
static const __FlashStringHelper* flashStepText(TwibootStep step) {
  switch (step) {
    case TwibootStep::ChipRequestFailed:  return F("chipinfo request failed");
    case TwibootStep::ChipShortRead:      return F("chipinfo read short");
    case TwibootStep::ChipBadSignature:   return F("unexpected chip signature");
    case TwibootStep::ChipBadPageSize:    return F("unexpected page size");
    case TwibootStep::PageNotReady:       return F("twiboot not ready before page");
    case TwibootStep::PageBurstTruncated: return F("page burst truncated");
    case TwibootStep::PageWriteFailed:    return F("page write failed");
    case TwibootStep::PageStuckBusy:      return F("twiboot stuck busy after page");
    case TwibootStep::PageReadFailed:     return F("verify read failed");
    default:                              return F("verify mismatch persisted");
  }
}

// Streams the PROGMEM-embedded unit firmware to one unit's twiboot. A false
// return leaves the reason in flashError.
namespace {
// loop()'s side of a unit flash: no abort path on this board, and a rewrite
// is not worth a log line of its 4 KB ring.
struct FlashWatch {
  bool keepGoing() { return true; }
  void pageRewritten(uint16_t, uint8_t) {}
};
}  // namespace

// Flashes the bundled image to one unit that sits in twiboot (shared
// unitFlashImage: size guard → liveness → chip check → pages → exit → sketch
// wait → restart). On failure `flashError` says why.
static bool flashUnitSteps(uint8_t i2cAddress) {
  FlashWatch watch;
  UnitFlashReport report = unitFlashImage(
      unitBus, i2cAddress, UNIT_FIRMWARE_BIN_LEN,
      [](size_t pageIndex, uint8_t* buf) {
        memcpy_P(buf, UNIT_FIRMWARE_BIN + pageIndex * TWIBOOT_PAGE_SIZE,
                 TWIBOOT_PAGE_SIZE);
      },
      watch);
  switch (report.result) {
    case UnitFlashResult::Ok:
      if (report.rebootStatus != 0) {
        SerialPrint(F("Unit "));
        SerialPrint(i2cAddress);
        SerialPrintln(F(": reboot command after flash not acked"));
      }
      return true;
    case UnitFlashResult::ImageTooLarge:
      flashError = F("image too large — would overwrite twiboot");
      break;
    case UnitFlashResult::BootloaderSilent:
      flashError = F("twiboot not responding");
      break;
    case UnitFlashResult::ChipMismatch:
    case UnitFlashResult::PageFailed:
      flashError = flashStepText(report.step);
      break;
    case UnitFlashResult::ExitFailed:
      flashError = F("exit bootloader failed");
      break;
    case UnitFlashResult::PostBootSilent:
      flashError = F("unit not responding post-flash");
      break;
    default:
      flashError = F("aborted");
      break;
  }
  return false;
}

static bool flashUnitFromProgmem(uint8_t i2cAddress) {
  flashError = "";
  bool ok = flashUnitSteps(i2cAddress);
  if (!ok) {
    SerialPrint(F("Unit "));
    SerialPrint(i2cAddress);
    SerialPrint(F(" flash failed: "));
    SerialPrintln(flashError);
  }
  return ok;
}

static bool waitForBatchIdle(const uint8_t* addrs, int count,
                             uint32_t timeoutMs) {
  return unitWaitBatchIdle(unitBus, addrs, count, timeoutMs, []() {});
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
// not a probe, and it never reads the offset. The unit facts then carried
// no offset for a whole freshly-flashed row whose calibration was perfectly
// intact, at the exact moment an operator checks it.
// The invalidation is right (UnitHealth.h's documented lifecycle: reads drop
// when a bootloader reboot invalidates them) — the repopulation was missing.
// This is what the S3 already does at the end of runReflashJob.
//
// The loop, its batch throttle and the #412 halt are the shared
// reflashRunTargets (ReflashPlan.h). `targets` is the planned list — already
// narrowed to one address for a targeted run (#513), so a unit sitting in
// twiboot at another address is not in it. Returns true when the sweep
// halted itself on consecutive failures.
namespace {
struct ReflashLoopHooks {
  bool stopRequested() { return false; }
  bool imageFits() { return twibootImageFits(UNIT_FIRMWARE_BIN_LEN); }
  bool inBootloader(uint8_t addr) { return isUnitInBootloader(addr); }
  int enterBootloader(uint8_t addr) {
    int status = busRebootToBootloader(addr);
    if (status == 0) busInvalidateUnitReads(addr);
    return status;
  }
  void pause(uint32_t ms) { delay(ms); }
  void unitNotEntered(uint8_t addr) {
    SerialPrint(F("Unit "));
    SerialPrint(addr);
    SerialPrintln(F(": not in its bootloader — not flashed"));
  }
  ReflashUnitOutcome flashUnit(uint8_t addr) {
    return flashUnitFromProgmem(addr) ? ReflashUnitOutcome::Flashed
                                      : ReflashUnitOutcome::Failed;
  }
  void unitFlashed(uint8_t addr) {
    UnitFacts& u = unitFacts[addr - SFP_I2C_ADDRESS_BASE];
    u.state = 1;
    strncpy(u.version, BUNDLED_UNIT_REV, 8);
    u.version[8] = '\0';
    u.fwStatus = 0;
  }
  void progressChanged() {}
  void settleBatch(const uint8_t* addrs, int n) {
    waitForBatchIdle(addrs, n, REFLASH_BATCH_SETTLE_MS);
  }
  void runHalted(uint8_t, int) {
    SerialPrintln(F("Unit reflash HALTED: consecutive failures — "
                    "remaining units left untouched"));
  }
};
}  // namespace

static bool flashBootloaderUnits(const uint8_t* targets, int count) {
  ReflashLoopHooks hooks;
  ReflashRunEnd end =
      reflashRunTargets(hooks, targets, count, reflashProgress);
  bool halted = end.halted;
  int flashed = end.flashed;
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
  return halted;
}

// The boot passes: the units already in twiboot, plus `sweep` (sketch units
// to bring up to date). Each unit enters its bootloader in the flash loop.
#if SERIAL_ENABLE == false
static void runBootFlash(const uint8_t* sweep, int sweepCount) {
  uint8_t targets[UNITS_AMOUNT];
  int n = reflashPlanTargets(unitFacts, UNITS_AMOUNT, SFP_I2C_ADDRESS_BASE,
                             sweep, sweepCount, targets);
  if (n == 0) return;
  reflashProgressBegin(reflashProgress, n);
  bool halted = flashBootloaderUnits(targets, n);
  reflashProgressFinish(reflashProgress, false, halted);
}
#endif

void busAutoInstallBootloaderUnits() {
#if SERIAL_ENABLE == false
  runBootFlash(nullptr, 0);
#endif
}

void busAutoUpdateOutdatedUnits() {
#if SERIAL_ENABLE == false
  uint8_t outdated[UNITS_AMOUNT];
  int n = reflashCollectOutdatedTargets(unitFacts, UNITS_AMOUNT,
                                        SFP_I2C_ADDRESS_BASE, outdated);
  if (n == 0) return;
  runBootFlash(outdated, n);
#endif
}

MaintGrade busLastReflashGrade() {
  MaintGrade grade{MaintOutcome::Ok, MaintReason::None};
  grade.outcome = classifyReflashOutcome(reflashProgress, grade.reason);
  return grade;
}

#if SERIAL_ENABLE == false
static bool runBootSweep(uint8_t onlyAddr);
#endif

void busRunReflashJob(uint8_t onlyAddr, bool force) {
#if SERIAL_ENABLE == false
  SerialPrintln(F("Unit reflash starting (throttled)..."));
  // The gate closes here and reopens at the single Finish below: every wait
  // in between yields to the web handlers, and a firmware upload let in
  // halfway would restart a row that has units in twiboot.
  reflashProgressBegin(reflashProgress, 0);  // total known once planned
  // Let the row finish what it was doing before the first unit leaves for
  // its bootloader. Idle, not homed — a home is a full turn per unit and
  // nothing here needs one (UnitUpdateJob.h).
  {
    uint8_t rowUnits[UNITS_AMOUNT];
    int rowCount = unitUpdateCollectSketchUnits(unitFacts, UNITS_AMOUNT,
                                                SFP_I2C_ADDRESS_BASE, rowUnits);
    if (rowCount > 0 &&
        !waitForBatchIdle(rowUnits, rowCount, UNIT_UPDATE_QUIET_MS)) {
      SerialPrintln(F("reflash: row still moving after the quiet wait"));
    }
  }
  // The flash list: the sweep's sketch units and whoever already sits in
  // twiboot. The loop sends each unit into its bootloader right before its
  // own pages (reflashEnterUnit).
  uint8_t sweep[UNITS_AMOUNT];
  int sweepCount =
      force ? reflashCollectForcedTarget(unitFacts, UNITS_AMOUNT,
                                         SFP_I2C_ADDRESS_BASE, onlyAddr, sweep)
            : reflashCollectRebootTargets(unitFacts, UNITS_AMOUNT,
                                          SFP_I2C_ADDRESS_BASE, sweep);
  uint8_t flashTargets[UNITS_AMOUNT];
  int n = reflashPlanTargets(unitFacts, UNITS_AMOUNT, SFP_I2C_ADDRESS_BASE,
                             sweep, sweepCount, flashTargets);
  n = reflashFilterToAddress(flashTargets, n, onlyAddr);
  reflashProgress.total = (uint8_t)n;
  bool halted = flashBootloaderUnits(flashTargets, n);
  reflashProgressSettling(reflashProgress);
  jobPollHealth();
  // Staggered boot-home of the just-flashed units (#309): a reflashed unit
  // reboots UNHOMED, so without this the next cluster render would home the
  // whole row at once — the #305 inrush class. Targets only unhomed units.
  followerBootHome();
  // Boot sections last (UnitUpdateJob.h): the flashed units are back in
  // their sketch and homed, the state stage 2 needs, so it costs no further
  // turn of the drum. A halted flash run does not start a second kind of
  // write.
  bool bootHalted = !halted && runBootSweep(onlyAddr);
  // The result goes out with the facts it is judged by: homed state from the
  // boot-home above (whose own poll stands down while the gate is closed)
  // and the new boot verdicts.
  jobPollHealth();
  reflashProgressFinish(reflashProgress, false, halted || bootHalted);
  SerialPrintln(F("Unit reflash complete."));
#else
  (void)onlyAddr;
  (void)force;
#endif
}

// In-system twiboot update (#499): the sequence, its timeouts and its grading
// are shared/BootUpdateOp.h; these are the superloop's hooks into it.
#if SERIAL_ENABLE == false
struct BootUpdateHooks {
  bool readBootInfo(uint8_t addr, BootUpdateReport& out) {
    return busReadBootInfo(addr, out);
  }
  bool waitIdle(uint8_t addr, uint32_t timeoutMs) {
    return waitForBatchIdle(&addr, 1, timeoutMs);
  }
  int sendStage(uint8_t addr, uint8_t stage) {
    return busBootUpdate(addr, stage);
  }
  int home(uint8_t addr) { return busHome(addr); }
  bool isHomed(uint8_t addr) {
    UnitStatus s;
    return unitReadStatus(unitBus, addr, s) && (s.flags & UNIT_FLAG_HOMED) != 0;
  }
  void unitLeftSketch(uint8_t addr) {
    busInvalidateUnitReads(addr);
    busArmProbeInhibit(millis() + UNIT_PROBE_INHIBIT_MS);
  }
  void holdProbes() { busArmProbeInhibit(millis() + UNIT_PROBE_INHIBIT_MS); }
  void pause(uint32_t ms) { delay(ms); }
  uint32_t nowMs() { return millis(); }
  void reshow() { reshowPending = lastFrameValid; }
  void note(BootUpdateStep step, MaintReason why, const BootUpdateReport&) {
    if (why == MaintReason::None) return;
    SerialPrint(F("boot-update step "));
    SerialPrint((int)step);
    SerialPrint(F(" -> "));
    SerialPrintln(maintReasonName(why));
  }
};
#endif

#if SERIAL_ENABLE == false
namespace {
struct BootSweepHooks {
  bool stopRequested() { return false; }
  MaintGrade bootUpdate(uint8_t addr) {
    BootUpdateHooks hooks;
    MaintGrade grade = bootUpdateRun(hooks, addr);
    SerialPrint(F("boot section of unit "));
    SerialPrint((int)addr);
    SerialPrint(F(" -> "));
    SerialPrintln(maintOutcomeName(grade.outcome));
    return grade;
  }
  void progressChanged() {}
  void sweepHalted(uint8_t, int) {
    SerialPrintln(F("Boot sweep HALTED: consecutive failures — remaining "
                    "units left untouched"));
  }
};
}  // namespace

// Brings the boot section of every unit on the bundled firmware to the
// current image (0 = the whole row); true when the sweep halted itself. It
// adds to the progress object's counters and leaves the Finish to its caller,
// so the gate does not reopen between the sweep and whatever follows it.
static bool runBootSweep(uint8_t onlyAddr) {
  uint8_t targets[UNITS_AMOUNT];
  int n = unitUpdateCollectBootTargets(unitFacts, UNITS_AMOUNT,
                                       SFP_I2C_ADDRESS_BASE, targets);
  n = reflashFilterToAddress(targets, n, onlyAddr);
  if (n == 0) return false;
  SerialPrint(F("boot sections to update: "));
  SerialPrintln(n);
  BootSweepHooks hooks;
  BootSweepEnd end = unitUpdateRunBootSweep(hooks, targets, n, reflashProgress);
  return end.halted;
}
#endif

void busAutoUpdateBootSections() {
#if SERIAL_ENABLE == false
  // On top of what the boot flash passes recorded: their counters stay, and
  // a row with nothing to update leaves the progress object alone.
  uint8_t targets[UNITS_AMOUNT];
  if (unitUpdateCollectBootTargets(unitFacts, UNITS_AMOUNT,
                                   SFP_I2C_ADDRESS_BASE, targets) == 0) {
    return;
  }
  bool halted = runBootSweep(0);
  jobPollHealth();  // publish the new boot verdicts with the result
  reflashProgressFinish(reflashProgress, false,
                        halted || reflashProgress.halted);
#endif
}

void busRunBootUpdate(uint32_t seq, uint8_t addr, MaintResult& result) {
#if SERIAL_ENABLE == false
  BootUpdateHooks hooks;
  MaintGrade grade = bootUpdateRun(hooks, addr);
  result = {seq, grade.outcome, grade.reason};
#else
  (void)seq; (void)addr; (void)result;
#endif
}

void busInvalidateUnitReads(uint8_t i2cAddress) {
  int idx = i2cAddress - SFP_I2C_ADDRESS_BASE;
  if (idx < 0 || idx >= UNITS_AMOUNT) return;
  unitFactsInvalidateReads(unitFacts[idx]);
}

#if SERIAL_ENABLE == false
namespace {
// The superloop's hooks into the shared boot-section dump (BootDumpOp.h).
struct BootDumpHooks {
  int enterBootloader(uint8_t addr) { return busRebootToBootloader(addr); }
  void unitLeftSketch(uint8_t addr) { busInvalidateUnitReads(addr); }
  void holdProbes() { busArmProbeInhibit(millis() + UNIT_PROBE_INHIBIT_MS); }
  void pause(uint32_t ms) { delay(ms); }
  UnitBootReadResult readBootSection(uint8_t addr, uint8_t* out) {
    bool exitAcked = false;
    bool answeredAfter = false;
    UnitBootReadResult r = unitReadBootSection(unitBus, addr, out, []() {},
                                               exitAcked, answeredAfter);
    if (r != UnitBootReadResult::BootloaderSilent && !exitAcked) {
      SerialPrint(F("Unit "));
      SerialPrint(addr);
      SerialPrintln(F(": twiboot exit failed after boot-section read"));
    }
    return r;
  }
  bool waitIdle(uint8_t addr, uint32_t timeoutMs) {
    return waitForBatchIdle(&addr, 1, timeoutMs);
  }
  int home(uint8_t addr) { return busHome(addr); }
  void reshow() { reshowPending = lastFrameValid; }
};
}  // namespace
#endif

void busRunBootDump(uint32_t seq, uint8_t addr,
                    BootDumpSlot& slot, uint8_t* outBytes) {
#if SERIAL_ENABLE == false
  slot.seq = seq;
  slot.addr = addr;
  BootDumpHooks hooks;
  slot.outcome = bootDumpRun(hooks, addr, outBytes);
  if (slot.outcome == BootDumpOutcome::Ok) {
    slot.crc32 = bootDumpCrc32(outBytes, BOOT_SECTION_LEN);
  }
  char logBuf[72];
  snprintf(logBuf, sizeof(logBuf), "boot-dump unit 0x%02x -> %s (crc32 %08lx)",
           addr, bootDumpOutcomeName(slot.outcome), (unsigned long)slot.crc32);
  SerialPrintln(logBuf);
#else
  (void)seq; (void)addr; (void)slot; (void)outBytes;
#endif
}
