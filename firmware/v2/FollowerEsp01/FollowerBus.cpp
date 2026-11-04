// FollowerBus.cpp — I2C unit bus glue (#298). Contract + caller rules in
// FollowerBus.h; straight port of v1's ServiceFlapFunctions.ino +
// ServiceFirmwareFunctions.ino onto the v2 UnitFacts model (blocking
// transactions and timing preserved — the Nanos are unchanged v1 hardware).
// This file: init, line liveness, bus-death recovery, the heartbeat scheduler,
// probe and health poll. The render path is FollowerBusRender.cpp, the
// single-unit ops FollowerBusOps.cpp, rescue and the twiboot jobs
// FollowerBusFlash.cpp; what they share is FollowerBusInternal.h.

#include "FollowerBus.h"
#include "FollowerBusInternal.h"

#include <Wire.h>

#include "BuildVersion.h"  // GIT_REV + BUNDLED_UNIT_REV (build_assets.py)
#include "FollowerBusRecovery.h"  // pure row-wide bus-death policy (#488)
#include "DisplayWidth.h"
#include "FollowerConfig.h"
#include "FollowerUnitJobs.h"  // unitJobHoldsRenders (#638)
#include "HeartbeatPolicy.h"  // pure heartbeat miss/schedule logic (#310)
#include "SplitFlapProtocol.h"
#include "TwibootFlash.h"  // the twiboot client; this family is its Wire adapter
#include "UnitBusCore.h"   // the unit sketch protocol, on the same adapter
#include "UnitProtocolHelpers.h"
#include "UnitTimings.h"
#include "BootUpdateOp.h"     // BOOT_CURRENT_CRC32 in the boot verdict note
#include "FollowerScanLog.h"  // which units a scan logs
#include "FollowerBusDeath.h"  // which units count, the record of a bus death (#496)
#include "FollowerEvents.h"
#include "FollowerLineTest.h"
#include "wall_link.pb.h"     // RowEventCode

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
uint32_t busTxCount = 0;
uint32_t busErrCount = 0;
uint32_t followerBusTxCount() { return busTxCount; }
uint32_t followerBusErrCount() { return busErrCount; }

// Per-unit error attribution (#367), as on the S3.
UnitErrorLedger<UNITS_AMOUNT> unitErrors;

namespace {
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
BusRecoveryState busRecovery;
bool reshowPending = false;
String lastFrame;
int lastFrameSpeed = 0;
bool lastFrameValid = false;
// Attempt lines logged per episode; a bus held for good then goes quiet
// instead of evicting the ring (the #436 flood lesson).
#define BUS_RECOVERY_LOGGED_ATTEMPTS 3

const BusRecoveryState& followerBusRecovery() { return busRecovery; }

// What a bus death is compared with (#496): the lines as last measured while
// units answered, and when this row last started a move.
static FollowerLineReading healthyLines;
uint32_t lastMoveMs = 0;
bool everMoved = false;
// How often the lines are measured on a working bus.
#define BUS_LINES_HEALTHY_EVERY_MS 600000UL

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

static void logLines(const __FlashStringHelper* what, const FollowerLineReading& r) {
  char sda[16], scl[16];
  followerLineText(sda, sizeof(sda), r.sda);
  followerLineText(scl, sizeof(scl), r.scl);
  SerialPrint(what);
  SerialPrint(F(" SDA "));
  SerialPrint(sda);
  SerialPrint(F(", SCL "));
  SerialPrintln(scl);
}

// On a working, idle bus: the reading a death is held against.
static void busLinesHealthyTick(uint32_t now) {
  static uint32_t measuredMs = 0;
  static bool measured = false;
  if (busRecovery.dead || followerUnitsAnswering(unitFacts, UNITS_AMOUNT) == 0) return;
  if (measured && now - measuredMs < BUS_LINES_HEALTHY_EVERY_MS) return;
  if (busRowMoving()) return;
  const bool first = !measured;
  measured = true;
  measuredMs = now;
  const FollowerLineReading r = followerLineTest();
  if (r.sda == FOLLOWER_LINE_NOT_MEASURED || r.scl == FOLLOWER_LINE_NOT_MEASURED) return;
  healthyLines = r;
  if (first) logLines(F("bus: on the working bus the lines rise in"), r);
}

// Once per episode, at its first recovery attempt (#496): what the bus looks
// like now, into the log and to the master (FollowerBusDeath.h). Every
// address a unit can have is asked for an acknowledge only.
static void recordBusDeath(uint8_t lineState, int hadAnswered) {
  FollowerBusDeath d;
  d.lineState = lineState;
  d.hadAnswered = (uint8_t)hadAnswered;
  if (everMoved) d.sinceMoveS = (millis() - lastMoveMs) / 1000UL;
  // Only with both lines free: under a held line every address would be
  // refused, each after the bus's own wait for the line.
  if (lineState == 0) {
    for (int i = 0; i < UNITS_AMOUNT; i++) {
      Wire.beginTransmission(toI2cAddress(i));
      const int status = Wire.endTransmission();
      if (status == 0) d.acked++;
      else if (status == 2) d.notAcked++;
      else d.refused++;
      yield();
    }
  }
  const FollowerLineReading lines = followerLineTest();
  char line[136];
  snprintf_P(line, sizeof(line),
             PSTR("bus: at its death %u unit(s) had answered; of %u addresses %u acknowledge, "
                  "%u do not, %u refused; last move "),
             (unsigned)d.hadAnswered, (unsigned)UNITS_AMOUNT, (unsigned)d.acked,
             (unsigned)d.notAcked, (unsigned)d.refused);
  SerialPrint(line);
  if (everMoved) {
    SerialPrint(d.sinceMoveS);
    SerialPrintln(F(" s ago"));
  } else {
    SerialPrintln(F("none since the start"));
  }
  logLines(F("bus: the lines now rise in"), lines);
  logLines(F("bus: on the working bus they rose in"), healthyLines);
  const uint32_t nowS = millis() / 1000UL;
  followerEvents().put(wl_RowEventCode_ROW_EVT_BUS_DEAD, 0, followerBusDeathA(d),
                       followerBusDeathB(d), nowS);
  followerEvents().put(wl_RowEventCode_ROW_EVT_BUS_LINES, 0,
                       followerLinesPack(lines.sda, lines.scl),
                       followerLinesPack(healthyLines.sda, healthyLines.scl), nowS);
}

// Clocks a slave-held SDA free: Wire.status() reads up to 20 bits, releasing
// a Nano stuck mid-byte, and leaves both lines released so the next START
// resets every slave's TWI state machine. No Wire.begin() re-init — Twi::init
// re-registers core timers/tasks, and the pins never change mode. SCL held low
// is reported but cannot be fixed from this side: the Nano needs a power cycle.
static void followerBusRecoveryTick() {
  uint32_t now = millis();
  if (reshowPending && !unitJobHoldsRenders()) {
    reshowPending = false;
    busShowSegment(String(lastFrame), lastFrameSpeed);
  }
  if (!busRecoveryDue(busRecovery, now)) return;
  uint8_t status = Wire.status();
  const int answering = followerUnitsAnswering(unitFacts, UNITS_AMOUNT);
  busRecoveryNoteAttempt(busRecovery, now, status);
  if (busRecovery.attemptsThisEpisode == 1) recordBusDeath(status, answering);
  if (busRecovery.attemptsThisEpisode <= BUS_RECOVERY_LOGGED_ATTEMPTS) {
    SerialPrint(F("bus: recovery attempt "));
    SerialPrint(busRecovery.attemptsThisEpisode);
    SerialPrint(F(", line state "));
    SerialPrintln(status);
  }
  // An empty row has no liveness reads to close the episode: re-probe
  // (quietly — this repeats every backoff step). What it finds is left to
  // the heartbeat's status reads to confirm (FollowerBusRecovery.h).
  // A row is empty while none of its units has answered: an address that
  // only acknowledged at a scan does not make it a row (FollowerBusDeath.h).
  if (busRecoveryReprobeDue(busRecovery, answering)) {
    const int before = detectedUnitCount;
    busProbeQuiet(true);
    if (detectedUnitCount != before) {
      SerialPrint(F("bus: re-probe found "));
      SerialPrint(detectedUnitCount);
      SerialPrintln(F(" unit(s)"));
    }
    // A unit in its bootloader, or one whose version was read, sent a reply
    // that checked out: that is a read that answered. Neither need be a unit
    // the heartbeat polls (a bootloader has no status, another protocol is
    // not read), so the episode closes here.
    bool unitReplied = false;
    for (int i = 0; i < UNITS_AMOUNT; i++) {
      if (unitFacts[i].state == 2 || unitFacts[i].protocolKnown) unitReplied = true;
    }
    if (unitReplied) {
      observeLiveness(0, true);
    } else {
      busRecoveryNoteReprobe(busRecovery, detectedUnitCount);
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
void pollHealthAll() {
  uint32_t now = millis();
  for (int i = 0; i < UNITS_AMOUNT; i++) {
    bool ok = busPollHealthOne(i);
    heartbeatApply(unitFacts[i], ok, now, HEARTBEAT_MISS_THRESHOLD);
  }
}
#endif

void busPollHealth() {
#if SERIAL_ENABLE == false
  if (reflashInProgress(reflashProgress)) return;
  pollHealthAll();
#endif
}

void followerHeartbeatTick() {
#if SERIAL_ENABLE == false
  settleReadTick();
  static uint32_t lastMs = 0;
  static int slot = 0;
  uint32_t now = millis();
  if (now - lastMs < HEARTBEAT_TICK_MS) return;
  lastMs = now;
  // Opportunistic + low priority (#310): never touch the bus while a unit may
  // be in its twiboot window (v1 #88) or a reflash is streaming.
  if ((int32_t)(now - busProbeInhibitedUntilMs()) < 0) return;
  if (reflashInProgress(reflashProgress)) return;
  if (followerUnitsAnswering(unitFacts, UNITS_AMOUNT) == 0) {
    busRecoveryNoteEmptyRow(busRecovery, now);
  }
  followerBusRecoveryTick();
  busLinesHealthyTick(now);
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
