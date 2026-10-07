// UnitBus.cpp (#203) — straight port of v1's ServiceFlapFunctions.ino bus
// core onto the S3 (same blocking Wire transactions, same timing constants;
// see UnitBus.h for the ownership rules). v1's volatile staging flags
// (busReprobePending, i2cBusBusy, ...) have no counterpart here: the
// display command queue serializes everything.

#include "UnitBus.h"

#include "BootDump.h"  // BOOT_SECTION_START / _LEN (#511)
#include "BootInfo.h"  // bootInfoStateName (#499)

#include <Arduino.h>
#include <Wire.h>

#include <atomic>

#include "BuildVersion.h"  // BUNDLED_UNIT_REV (#205)
#include "CrashContext.h"  // per-transaction crash breadcrumb (#504)
#include "HelpersSerialHandling.h"
#include "MaintenancePolicy.h"
#include "UnitTimings.h"
#include "MotionBudget.h"  // motion admission (#505)
#include "RenderStagger.h"  // sub-frame inrush stagger (#324)
#include "SplitFlapProtocol.h"
#include "TaskWatchdog.h"  // wdtFeed() (#314)
#include "TwibootFlash.h"  // the twiboot client; this file is its Wire adapter
#include "UnitBusCore.h"   // the unit sketch protocol, on the same adapter
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
// 100 kHz standard-mode (#383 reverted #375's 400 kHz): the reflash path shares
// this single clock and twiboot is only proven at standard mode, so driving the
// bootloader at 400 kHz makes flash writes unreliable; 400 kHz signal integrity
// on the full 16-unit wall also proved marginal (units dropped, bus locked on a
// master reboot). The Nano TWI sketch slaves follow whatever the master clocks.
// Re-raising requires dropping to 100 kHz for the twiboot phase (or a validated
// Fast-mode reflash) — see #383; #367 per-unit error telemetry stays the guard.
static constexpr uint32_t UNIT_BUS_FREQ_HZ = 100000;



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
  crashCtxMark(CRASH_SLOT_DISPLAY, CRASH_ACT_I2C_RECOVER);
  Wire.end();
  unitBusInit();
}

// Bus transaction counters for the board page (#245). displayTask is the
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

// Per-unit I2C error attribution (#367). busErrCount above is fleet-global — it
// can't say WHICH unit's transactions fail, the exact signal the 400 kHz bump
// (#375) has to be validated against. These parallel per-address counters
// charge a failed render write or health-poll read to its column; displayTask
// folds them into the snapshot's UnitFacts (foldUnitErrors) so the unit facts
// attributes err/errAge per unit. Lifetime since boot — deliberately NOT reset
// by a probe rescan (a reliability trend, unlike the re-baselined health masks).
// displayTask is the sole writer (sole Wire toucher, Hard rules), so plain
// arrays need no atomics — the netTask reader only ever sees the folded snapshot.
static UnitErrorLedger<UNITS_AMOUNT> unitErrors;

static void noteUnitError(int index) { unitErrors.note(index, millis()); }

// Copies the per-unit error counters into the caller's facts so they ride the
// next published snapshot. Runs on every health poll: cheap, and keeps a
// render-time error visible within one heartbeat tick regardless of which
// unit the round-robin polled.
static void foldUnitErrors(UnitFacts* facts, int n) { unitErrors.fold(facts, n); }

// The unit protocol is shared/UnitBusCore.h and the twiboot protocol
// shared/TwibootFlash.h; this is the Wire adapter both run on. A short read
// must pass through recoverBusAfterFailedRead() before the bus is touched
// again (#207); write NACKs during a page-program window are expected and are
// not bus damage.
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
  void readFailed() { recoverBusAfterFailedRead(); }
  int endCounted() {
    int status = Wire.endTransmission();
    busTxCount.fetch_add(1);
    if (status != 0) busErrCount.fetch_add(1);
    return status;
  }
  void noteReadError() { busErrCount.fetch_add(1); }  // short read leg (#245)
  void mark(UnitBusAct act, uint8_t addr) {
    crashCtxMark(CRASH_SLOT_DISPLAY,
                 act == UnitBusAct::Write   ? CRASH_ACT_I2C_WRITE
                 : act == UnitBusAct::Read ? CRASH_ACT_I2C_READ
                                           : CRASH_ACT_I2C_PROBE,
                 addr);
  }
};
WireTwibootBus unitBus;
}  // namespace

// What the shared probe/poll code found worth a log line.
namespace {
struct UnitBusNotes {
  // Completes the scan-log entry unitBusProbe opened with "- unit at 0x..".
  void identityRead(uint8_t, const UnitFacts& fact, bool versionReadable) {
    if (!versionReadable) {
      SerialPrintln(F(" is running sketch (fw UNKNOWN — unreadable version reply)"));
    } else if (!unitProtocolSupported(fact.protocolVersion)) {
      SerialPrintf(" speaks protocol v%u, we speak v%u — NOT DRIVABLE, reflash target\n",
                   (unsigned)fact.protocolVersion,
                   (unsigned)SFP_PROTOCOL_VERSION);
    } else {
      SerialPrintf(" is running sketch (fw %s%s)\n", fact.version,
                   fact.fwStatus == 0 ? "" : " — OUTDATED");
    }
  }
  // The unit's drift auto re-home (#263) is otherwise silent on the operator
  // log — it only prints to the Nano's own (unmonitored) serial (#322).
  void driftSeen(uint8_t i2cAddress, const DriftLogDecision& drift,
                 const UnitDiagReading& d) {
    SerialPrintf("Unit 0x%02x drifted: %u new event(s), last %d steps "
                 "(de=%u) — unit auto re-homing\n",
                 i2cAddress, (unsigned)drift.newEvents, (int)d.lastDriftSteps,
                 (unsigned)d.driftEvents);
  }
  void bootVerdictChanged(uint8_t i2cAddress, const UnitFacts& fact,
                          const BootUpdateReport& r) {
    SerialPrintf("Unit 0x%02x bootloader %s — crc32 %08lx (expected %08lx), "
                 "state %s\n",
                 i2cAddress, bootIntegrityName(fact.bootVerdict),
                 (unsigned long)r.bootCrc32, (unsigned long)BOOT_CURRENT_CRC32,
                 bootInfoStateName(r.state));
  }
};
UnitBusNotes unitBusNotes;
}  // namespace

// Per-unit memory of the last boot verdict logged (#520): outside the facts,
// so a probe rescan (which rebuilds them) does not repeat a finding.
static uint8_t bootVerdictLogged[UNITS_AMOUNT];

static bool isUnitInBootloader(int i2cAddress) {
  unitBus.mark(UnitBusAct::Probe, (uint8_t)i2cAddress);
  return twibootIsBootloader(unitBus, (uint8_t)i2cAddress);
}

// Checks if a single unit is moving (1-byte rotation status). Called ~10x/s
// from isDisplayMoving() — must be quiet in the log when nothing is wrong.
static int checkIfMoving(int unitIndex) {
  return unitMovingStatus(unitBus, (uint8_t)toI2cAddress(unitIndex));
}

// True while any sketch-mode unit reports rotation. A silent read (-1) from
// such a unit is treated as "idle" rather than "sleeping" so the master
// never deadlocks on a transiently unresponsive (or physically absent) unit.
static bool isDisplayMoving(const UnitFacts* facts, int width) {
  for (int unitIndex = 0; unitIndex < width; unitIndex++) {
    if (!unitDrivable(facts[unitIndex])) continue;  // #405
    if (checkIfMoving(unitIndex) == 1) return true;
  }
  return false;
}

// --- motion admission (#505, MotionBudget.h) ---------------------------------
// The cap is owned by displayTask (sag-adaptive) and pushed in; the gate is
// displayTask's radio-quiet wait, called before any op that starts motion.
static int motionCap = MOTION_BUDGET_MAX;
static void (*motionGate)() = nullptr;

void unitBusSetMotionCap(int cap) { motionCap = cap; }
void unitBusSetMotionGate(void (*gate)()) { motionGate = gate; }

static void admitMotion() {
  if (motionGate) motionGate();
}

// Blocks until fewer than motionCap tracked units are still moving. Returns
// false when the stop action aborted the wait.
static bool waitForMotionSlot(MotionTracker& movers) {
  while (motionTrackerFull(movers, motionCap)) {
    wdtFeed();
    if (abortRequested.load()) return false;
    for (int k = movers.count - 1; k >= 0; k--) {
      motionTrackerObserve(movers, k, checkIfMoving(movers.unit[k]), millis());
    }
    if (motionTrackerFull(movers, motionCap)) delay(50);
  }
  return true;
}

// Waits until no unit reports rotation, with the UNIT_SHOW_STUCK_TIMEOUT_MS
// stuck-unit cap. The delay(100) yields displayTask's core between polls.
// The abort signal (#204) short-circuits the wait so a queued Stop takes
// effect promptly instead of sitting out a stuck-unit timeout.
static void waitForDisplayToStop(const UnitFacts* facts, int width) {
  uint32_t waitStart = millis();
  uint32_t lastWaitLog = 0;
  while (isDisplayMoving(facts, width)) {
    wdtFeed();  // #314: feed the TWDT through a legitimate stuck-flap wait
    if (abortRequested.load()) {
      SerialPrintln(F("Display-stop wait aborted by stop"));
      break;
    }
    if (millis() - waitStart > UNIT_SHOW_STUCK_TIMEOUT_MS) {
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
  MotionTracker movers;  // resends are moves too (#505)
  for (int unitIndex = 0; unitIndex < width; unitIndex++) {
    if (!unitDrivable(facts[unitIndex])) continue;  // #405
    int shown;
    if (!unitReadDisplayedLetter(unitBus, (uint8_t)toI2cAddress(unitIndex),
                                 shown)) {
      continue;
    }
    if (shown == letters[unitIndex]) continue;
    SerialPrintf("Unit %d shows the wrong letter index — re-sending\n",
                 unitIndex);
    if (!waitForMotionSlot(movers)) break;
    unitWriteLetter(unitBus, (uint8_t)toI2cAddress(unitIndex),
                    letters[unitIndex], unitSpeed);
    motionTrackerAdd(movers, unitIndex, millis());
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
    crashCtxMark(CRASH_SLOT_DISPLAY, CRASH_ACT_I2C_PROBE, (uint8_t)i2cAddress);
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
      // Which bootloader, and its lock and fuse bytes where it serves them
      // (#541/#543). The chipinfo probe above already holds its countdown.
      twibootReadIdentity(unitBus, (uint8_t)i2cAddress,
                          facts[unitIndex].bootloader);
      char identity[TWIBOOT_IDENTITY_TEXT_CAP];
      twibootIdentityText(identity, sizeof(identity),
                          facts[unitIndex].bootloader);
      SerialPrintf(" is in BOOTLOADER mode%s\n", identity);
      continue;
    }
    UnitFacts& fact = facts[unitIndex];
    unitProbeSketchUnit(unitBus, unitBusNotes, fact, (uint8_t)i2cAddress,
                        BUNDLED_UNIT_REV, BUNDLED_UNIT_REV_EQUIV,
                        bootVerdictLogged[unitIndex]);
  }
  // #367: the per-unit reset above zeroed the facts' error fields, but the
  // attributed counters are lifetime — restore them so a probe rescan doesn't
  // drop the reliability trend (only a reboot clears them).
  foldUnitErrors(facts, maxUnits);
  SerialPrintf("I2C scan complete. Detected %d", detected);
  SerialPrintf("/%d possible units.\n", maxUnits);
}

// Per-unit reset-counter baselines (UnitHealth.h). Outside the facts so a
// probe rescan, which rebuilds every slot, does not re-baseline a unit that
// reset; only a master reboot does.
static UnitResetBaseline resetBaselines[UNITS_AMOUNT];

bool unitBusPollHealthOne(UnitFacts* facts, int i) {
  facts[i].statusValid = false;
  facts[i].bootVerdict = BOOT_INTEGRITY_UNREAD;
  // #367: refresh every column's attributed error counters into the facts BEFORE
  // the state gate below, so a render-time write failure (charged in
  // unitBusShowFrame) surfaces in the next published unit facts within one
  // heartbeat tick — even on a tick whose round-robin slot lands on a silent /
  // bootloader unit that returns early.
  foldUnitErrors(facts, UNITS_AMOUNT);
  // Only sketch-running units (state 1) answer CMD_GET_STATUS; a unit in
  // bootloader (2) or silent (0) is left invalid so it renders as a gap
  // in the table and never counts toward the faulty total.
  // #405: a unit speaking an unrecognised contract is not polled either — its
  // reply layout is by definition unknown, so reading it would be guessing.
  if (!unitDrivable(facts[i])) return false;
  bool ok = unitPollHealth(unitBus, unitBusNotes, facts[i],
                           (uint8_t)toI2cAddress(i), resetBaselines[i],
                           bootVerdictLogged[i]);
  // #367: the heartbeat status read is the clean per-unit liveness probe —
  // charge its failure to this column (the diagnostic sub-reads are not
  // charged; they fail routinely on pre-opcode firmware).
  if (!ok) noteUnitError(i);
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
  admitMotion();

  int writeErrors = 0;
  int commanded = 0;
  bool aborted = false;
  MotionTracker movers;
  for (int unitIndex = 0; unitIndex < width; unitIndex++) {
    // Skip slots the probe did not find a sketch-running unit on: writing
    // to absent addresses stalls isDisplayMoving() and a dead unit
    // mid-display must not wedge the whole frame (v1 behavior). #405 also
    // skips a unit whose protocol version we do not speak — a render is the
    // loudest thing we could get wrong against an unknown contract.
    if (!unitDrivable(facts[unitIndex])) continue;
    // #505: at most motionCap units moving at once — the steady draw of every
    // energised stepper, not just the start spike, is what sags the rail.
    if (!waitForMotionSlot(movers)) {
      aborted = true;
      break;
    }
    // #324: spread the flap inrush — pause before opening each new group so a
    // full row's steppers don't spin up at once and brown out the rail.
    if (renderStaggerShouldSettle(commanded, RENDER_STAGGER_BATCH)) {
      delay(RENDER_STAGGER_SETTLE_MS);
    }
    if (unitWriteLetter(unitBus, (uint8_t)toI2cAddress(unitIndex),
                        letters[unitIndex], (uint8_t)unitSpeed) != 0) {
      writeErrors++;
      noteUnitError(unitIndex);  // #367: attribute the render write to its unit
    }
    motionTrackerAdd(movers, unitIndex, millis());
    commanded++;
  }

  waitForDisplayToStop(facts, width);
  // An aborted frame is abandoned, not repaired: the queued Stop parks it.
  if (!aborted) verifyAndResendLetters(facts, width, letters, (uint8_t)unitSpeed);
  return writeErrors;
}

// --- calibration + provisioning (#204) — straight v1 ports --------------------
// Payload encodings live in MaintenancePolicy.h so the negative int16/int8
// wire bytes are asserted natively.

// Value + bitwise complement (#405), then READ IT BACK. This was
// fire-and-forget: the master wrote and returned the transmission status, with
// no verification even though GET_OFFSET exists. Range-clamping on both sides
// bounded the damage but detected nothing.
//
// Returns 0 only when the unit's own GET_OFFSET confirms the value landed.
// The complement stops the unit persisting a corrupted write; the read-back
// stops the master reporting success for a write that never took.
int unitBusWriteOffset(int i2cAddress, int16_t value) {
  return unitWriteOffset(unitBus, (uint8_t)i2cAddress, value);
}

int unitBusJog(int i2cAddress, int steps) {
  admitMotion();
  return unitJog(unitBus, (uint8_t)i2cAddress, steps);
}

int unitBusHome(int i2cAddress) {
  admitMotion();
  return unitHome(unitBus, (uint8_t)i2cAddress);
}

int unitBusIdentify(int i2cAddress) {
  return unitIdentify(unitBus, (uint8_t)i2cAddress);
}

int unitBusResetOdometer(int i2cAddress) {
  return unitResetOdometer(unitBus, (uint8_t)i2cAddress);
}

// Feature gates (#409). The write half of the byte GET_LIFETIME reports, and
// the only one of the three complement-protected writes that can be confirmed
// afterwards — so it is, unlike SET_I2C_ADDRESS. The unit refuses gate bits it
// has no code for, which lands here as a MISMATCH rather than a silent 0.
int unitBusSetGates(int i2cAddress, uint8_t gates) {
  return unitSetGates(unitBus, (uint8_t)i2cAddress, gates);
}

int unitBusStartSelfTest(int i2cAddress) {
  admitMotion();
  return unitStartSelfTest(unitBus, (uint8_t)i2cAddress);
}

bool unitBusReadSelfTest(int i2cAddress, UnitSelfTestReading& out) {
  return unitReadSelfTest(unitBus, (uint8_t)i2cAddress, out);
}

bool unitBusReadBootInfo(int i2cAddress, BootUpdateReport& out) {
  return unitReadBootInfo(unitBus, (uint8_t)i2cAddress, out);
}

bool unitBusIsHomed(int i2cAddress) {
  UnitStatus s;
  return unitReadStatus(unitBus, (uint8_t)i2cAddress, s) &&
         (s.flags & UNIT_FLAG_HOMED) != 0;
}

int unitBusBootUpdate(int i2cAddress, uint8_t stage) {
  return unitSendBootUpdate(unitBus, (uint8_t)i2cAddress, stage);
}

// The unit watchdog-resets into twiboot, which listens ~1 s on the
// DIP-derived address. HARD RULE (v1 #88): never probe while a unit can be
// in its twiboot window — the CHIPINFO query pins the bootloader alive.
int unitBusRebootToBootloader(int i2cAddress) {
  return unitEnterBootloader(unitBus, (uint8_t)i2cAddress);
}

// Value + bitwise complement (#405). Cannot be verified afterwards by
// construction — the unit persists the address and reboots, so it is gone from
// the address we were talking to. That is exactly why the complement matters
// here more than anywhere else: a single-bit corruption landing inside 1..126
// used to relocate a unit to an address nobody was looking at, recoverable
// only by a physical trip to re-DIP (twiboot listens on the DIP address).
int unitBusSetAddress(int i2cAddress, uint8_t newAddress) {
  return unitSetAddress(unitBus, (uint8_t)i2cAddress, newAddress);
}

int unitBusClearAddress(int i2cAddress) {
  return unitClearAddress(unitBus, (uint8_t)i2cAddress);
}

// --- unit reflash over twiboot (#205) -----------------------------------------
bool unitBusIsBootloader(int i2cAddress) {
  return isUnitInBootloader(i2cAddress);
}

UnitRescueProbe unitBusRescueProbe(int i2cAddress, TwibootIdentity& id) {
  return unitRescueProbe(unitBus, (uint8_t)i2cAddress, id);
}

// The 132-byte page burst must fit the Wire TX buffer in one transaction —
// a smaller buffer makes Wire.write() silently drop the page tail and every
// verify fails (#243). The -DI2C_BUFFER_LENGTH=256 build flag provides it.
static_assert(I2C_BUFFER_LENGTH >= TWIBOOT_PAGE_SIZE + 4,
              "Wire buffer too small for a twiboot page write — "
              "build with -DI2C_BUFFER_LENGTH=256 (#243)");

const char* unitFlashResultName(UnitFlashResult r) {
  switch (r) {
    case UnitFlashResult::Ok:               return "ok";
    case UnitFlashResult::ImageTooLarge:    return "image-too-large";
    case UnitFlashResult::BootloaderSilent: return "bootloader-silent";
    case UnitFlashResult::ChipMismatch:     return "chip-mismatch";
    case UnitFlashResult::PageFailed:       return "page-failed";
    case UnitFlashResult::ExitFailed:       return "exit-failed";
    case UnitFlashResult::PostBootSilent:   return "post-boot-silent";
    default:                                return "aborted";
  }
}

bool unitBusWaitBatchIdle(const uint8_t* addrs, int count,
                          uint32_t timeoutMs) {
  if (count <= 0) return true;
  SerialPrintf("  waiting for %d unit(s) to come online + finish homing...\n",
               count);
  // #314: feed the TWDT through the batch-settle poll.
  bool idle = unitWaitBatchIdle(unitBus, addrs, count, timeoutMs,
                                []() { wdtFeed(); });
  SerialPrintln(idle ? F("  batch online + idle")
                     : F("  batch settle timed out — continuing anyway"));
  return idle;
}

UnitBootReadResult unitBusReadBootSection(int i2cAddress, uint8_t* out) {
  bool exitAcked = false;
  bool answeredAfter = false;
  UnitBootReadResult result = unitReadBootSection(
      unitBus, (uint8_t)i2cAddress, out, []() { wdtFeed(); }, exitAcked,
      answeredAfter);
  if (result == UnitBootReadResult::BootloaderSilent) return result;
  if (!exitAcked) {
    SerialPrintf("Unit 0x%02x did not ACK the twiboot exit after the "
                 "boot-section read\n", i2cAddress);
  } else if (!answeredAfter) {
    SerialPrintf("Unit 0x%02x not responding after the boot-section read\n",
                 i2cAddress);
  }
  return result;
}

namespace {
// displayTask's side of a unit flash: feed the task watchdog between pages
// (#314) and stop when the stop action asked for it.
struct FlashWatch {
  uint8_t addr;
  bool keepGoing() {
    wdtFeed();
    return !abortRequested.load();
  }
  void pageRewritten(uint16_t flashAddr, uint8_t rewrites) {
    SerialPrintf("Verify mismatch at 0x%02x page 0x%04x (%u write(s) read "
                 "back wrong)\n",
                 addr, flashAddr, (unsigned)rewrites);
  }
};
}  // namespace

UnitFlashResult unitBusFlashUnit(int i2cAddress, const uint8_t* image,
                                 size_t len) {
  SerialPrintf("Flashing unit at 0x%02x (%u bytes)\n", i2cAddress,
               (unsigned)len);
  const uint8_t addr = (uint8_t)i2cAddress;
  FlashWatch watch{addr};
  UnitFlashReport report = unitFlashImage(
      unitBus, addr, len,
      [image](size_t pageIndex, uint8_t* buf) {
        memcpy(buf, image + pageIndex * TWIBOOT_PAGE_SIZE, TWIBOOT_PAGE_SIZE);
      },
      watch);
  switch (report.result) {
    case UnitFlashResult::Ok:
      if (report.rebootStatus != 0) {
        SerialPrintf("Unit 0x%02x CMD_REBOOT failed (status %d)\n", i2cAddress,
                     report.rebootStatus);
      }
      SerialPrintf("Unit 0x%02x flashed (%u bytes) — sent CMD_REBOOT\n",
                   i2cAddress, (unsigned)len);
      break;
    case UnitFlashResult::ImageTooLarge:
      SerialPrintf("Unit image too large (%u > %u) — would overwrite twiboot\n",
                   (unsigned)len, (unsigned)BOOT_SECTION_START);
      break;
    case UnitFlashResult::ChipMismatch:
      SerialPrintf("Unit 0x%02x chip check failed: %s\n", i2cAddress,
                   twibootStepName(report.step));
      break;
    case UnitFlashResult::PageFailed:
      SerialPrintf("Unit flash FAILED at page 0x%04x (%s) — unit left in "
                   "twiboot\n",
                   report.pageAddr, twibootStepName(report.step));
      break;
    case UnitFlashResult::PostBootSilent:
      SerialPrintf("Unit 0x%02x not responding post-flash\n", i2cAddress);
      break;
    case UnitFlashResult::Aborted:
      SerialPrintln(F("Unit flash aborted by stop — unit left in twiboot"));
      break;
    default:
      break;
  }
  return report.result;
}
