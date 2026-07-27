// DisplayTask.cpp — the core-1 display domain (#187), split out of
// Tasks.cpp (#352). Exclusive owner of I2C/Wire (via UnitBus, #203) and the
// single snapshot writer. Contains the boot probe/boot-home sequence, the
// heartbeat tick, the #205 unit-reflash job and displayTaskMain's command
// dispatch (one static exec* helper per opcode, #353).

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "BootHomePlan.h"
#include "FlapFrame.h"
#include "HeadlessPolicy.h"
#include "HeartbeatPolicy.h"
#include "HelpersSerialHandling.h"
#include "TaskWatchdog.h"
#include "TasksInternal.h"
#include "UnitBus.h"
#include "UnitEventLog.h"  // per-unit health transition log decision (#322)
#include "ContentState.h"

// Settle after an address-mutating burn before the follow-up probe (#204):
// the unit watchdog-resets THROUGH its twiboot window (~1 s) and probing
// inside it pins the bootloader (v1 #88) — 3 s clears the window plus the
// homing start, same margin class as the 1500 ms boot delay.
static constexpr uint32_t ADDRESS_OP_SETTLE_MS = 3000;

// Self-test wait (#265): the unit's diagnostic is ~2 revolutions at homing
// speed (~12-15 s) but can queue behind a slow in-flight move unit-side, so
// the window is generous — and it re-arms once RUNNING is first observed,
// so the true worst-case displayTask block is ~2x this constant. Polled at
// 500 ms; three consecutive invalid replies with none ever valid =
// firmware predating the opcode.
static constexpr uint32_t SELF_TEST_TIMEOUT_MS = 45000;
static constexpr uint32_t SELF_TEST_POLL_MS = 500;
static constexpr int SELF_TEST_UNSUPPORTED_POLLS = 3;

// --- core 1: display domain ---------------------------------------------------

// Exclusive owner of I2C/Wire (via UnitBus, #203). Boot: bus up → twiboot
// settle → probe → health poll → publish. Loop: pure transition
// (displayApplyCommand) + the hardware work per opcode. Blocking bus
// operations run right here by design — commands queue behind them, and
// the published busy flag covers the whole execution.
// Probe inhibit after any op that reboots a unit THROUGH its twiboot window
// (v1 #88 hard rule: the probe's CHIPINFO query pins twiboot alive). Owned
// by displayTask exclusively; every runtime probe waits this deadline out —
// including a Probe that was already queued behind a /unit/reboot.
static uint32_t twibootRiskUntilMs = 0;

static void armTwibootRiskWindow() {
  twibootRiskUntilMs = millis() + ADDRESS_OP_SETTLE_MS;
}

static void settleBeforeProbe() {
  int32_t remaining = (int32_t)(twibootRiskUntilMs - millis());
  if (remaining > 0) delay((uint32_t)remaining);
}

// No-units detection (#329). displayTask owns this exclusively (single core-1
// task), so a plain static needs no lock. Fed only at the STEADY observation
// points — boot probe, explicit Probe, idle heartbeat tick — not the transient
// maintenance/reflash reprobes, so the streak tracks real bus cadence. Latches
// headlessUnitless after HEADLESS_ZERO_PROBE_THRESHOLD consecutive 0-unit
// reads; a single responding unit resets it (never flips a real display).
static HeadlessDetector headlessDetector;

static void headlessTrack(DisplaySnapshot& local) {
  local.headlessUnitless =
      headlessObserveProbe(headlessDetector, local.detectedUnitCount);
}

// --- heartbeat freshness + batched boot-home (#309/#310) ---------------------

// A full health poll + a freshness stamp for every slot (#310, HeartbeatPolicy).
// Used by boot, the explicit Probe and every post-op reprobe so a refresh
// resets the miss counters and makes /units/health "age" truthful immediately
// after. The read outcome per slot is its statusValid (set by unitBusPollHealth).
static void pollHealthWithFreshness(UnitFacts* busFacts) {
  unitBusPollHealth(busFacts, UNITS_AMOUNT);
  uint32_t now = millis();
  for (int i = 0; i < UNITS_AMOUNT; i++) {
    heartbeatApply(busFacts[i], busFacts[i].statusValid, now,
                   HEARTBEAT_MISS_THRESHOLD);
  }
}

// Batched boot-home (#309). The units boot UNHOMED; home the ones that still
// report unhomed in bounded batches with a rail-settle between them, so a
// whole row's steppers don't spike the shared rail at once (the #305
// verify-boot brownout). Targets only unhomed sketch units, so it serves both
// a cold boot (home all) and a post-reflash top-up (home just the flashed
// units) without re-homing good ones. Status-driven waits (homed-or-faulted)
// come from unitBusWaitBatchIdle; abort (/stop) bails between batches.
static void runBootHomeSequence(DisplaySnapshot& local, UnitFacts* busFacts) {
  uint8_t targets[UNITS_AMOUNT];
  int n = bootHomeCollectTargets(local.units, local.displayWidth,
                                 SFP_I2C_ADDRESS_BASE, targets);
  if (n == 0) return;
  SerialPrintf("boot-home: staggering %d unit(s) in batches of %d\n", n,
               BOOT_HOME_BATCH_SIZE);
  for (int i = 0; i < n; i += BOOT_HOME_BATCH_SIZE) {
    wdtFeed();  // #314: each batch waits on unit settle — keep the dog fed
    if (unitBusAbortRequested()) break;
    uint8_t batch[BOOT_HOME_BATCH_SIZE];
    int batchN = 0;
    for (int j = i; j < n && batchN < BOOT_HOME_BATCH_SIZE; j++) {
      unitBusHome(targets[j]);
      batch[batchN++] = targets[j];
    }
    // Status-driven: wait for each commanded unit to report homed-or-faulted
    // (not moving) before settling the rail for the next batch.
    unitBusWaitBatchIdle(batch, batchN, BOOT_HOME_BATCH_TIMEOUT_MS);
    delay(BOOT_HOME_SETTLE_MS);
  }
  // Re-poll so the published snapshot reflects the now-homed state.
  pollHealthWithFreshness(busFacts);
  displayApplyUnitFacts(local, busFacts, UNITS_AMOUNT,
                        effectiveWidthOverride());
  snapshotPublish(local);
}

// #322: surface a unit's health TRANSITIONS on the operator log. The master
// otherwise folds home-failed / hall-never / stale / mismatch into passive
// /units/health JSON, so a unit going bad (or recovering) never reaches the
// flash/web log an operator watches — the same silent gap as the drift auto
// re-home (logged in refreshUnitDiag). Evaluated per unit right after ITS own
// heartbeat poll, so every signal (incl. the #264 mismatch verdict, coherent
// only at diag-poll time per #267) describes this unit at this instant. The
// last-logged mask lives in busFacts (durable across polls; a probe rescan
// re-zeroes it) — pure edge logic in UnitEventLog.h. Unlike DriftLogPolicy's
// silent-first-read (drift is a count of PAST events, re-announcing history is
// noise), a health CONDITION is live state: logging it the first time it's seen
// — at boot, or re-asserted after a maintenance rescan — is the point (an
// operator wants "unit 3 is faulty" surfaced), so prior=0 onsets deliberately.
static void logUnitHealthTransition(const DisplaySnapshot& local,
                                    UnitFacts* busFacts, int i) {
  const UnitFacts& u = local.units[i];
  uint8_t cur = 0;
  // A condition is only OBSERVABLE this tick when its backing I2C read
  // succeeded — otherwise validMask carries the prior state instead of faking a
  // recovery + duplicate re-onset. mismatch rides the DIAG read (physKnown
  // needs diagValid, DisplayIpc.h), home/hall ride the STATUS read; the two are
  // separate transactions and can miss independently. STALE is the master's own
  // heartbeat verdict — always meaningful (it's SET when reads fail).
  uint8_t valid = UNIT_EVT_STALE;
  if (u.diagValid) valid |= UNIT_EVT_MISMATCH;
  if (u.statusValid) {
    valid |= UNIT_EVT_HOME_FAILED | UNIT_EVT_HALL_NEVER;
    if (u.status.flags & UNIT_FLAG_LAST_HOME_FAILED) cur |= UNIT_EVT_HOME_FAILED;
    if (u.status.flags & UNIT_FLAG_HALL_NEVER)       cur |= UNIT_EVT_HALL_NEVER;
  }
  // #366: low-Vcc rides the vitals read (its own transaction, can miss
  // independently). Onset-only (non-recoverable) — vccMin is a since-boot
  // minimum that only ever falls, so it "clears" solely on a unit reboot, which
  // re-baselines the whole mask at the next probe rescan.
  if (u.vitalsValid) {
    valid |= UNIT_EVT_LOW_VCC;
    if (unitVccIsLow(u.vitalsValid, u.vitals.vccMin_mV, UNIT_VCC_MIN_FLOOR_MV))
      cur |= UNIT_EVT_LOW_VCC;
  }
  if (u.stale)    cur |= UNIT_EVT_STALE;
  if (u.mismatch) cur |= UNIT_EVT_MISMATCH;

  // #365: jam/drag/hall-anomaly ride the GET_EXT_DIAG read (its own
  // transaction, can miss independently) — the builder folds all three
  // gated on extDiagValid, so a silent/pre-ext-diag unit contributes none.
  UnitEventTransitions t = unitEventEvaluate(busFacts[i].healthEventState, cur,
                                             valid, u.extDiagValid, u.extDiag);
  busFacts[i].healthEventState = t.newState;
  if (!t.onset && !t.recovery) return;

  int addr = SFP_I2C_ADDRESS_BASE + i;
  if (t.onset & UNIT_EVT_STALE)
    SerialPrintf("Unit 0x%02x LOST — no heartbeat, off the bus\n", addr);
  if (t.onset & UNIT_EVT_HOME_FAILED)
    SerialPrintf("Unit 0x%02x: last home FAILED (faulty)\n", addr);
  if (t.onset & UNIT_EVT_HALL_NEVER)
    SerialPrintf("Unit 0x%02x: hall sensor never fired (faulty)\n", addr);
  if (t.onset & UNIT_EVT_MISMATCH)
    SerialPrintf("Unit 0x%02x: displayed letter disagrees with intended (#264)\n",
                 addr);
  if (t.onset & UNIT_EVT_LOW_VCC)
    SerialPrintf("Unit 0x%02x: supply Vcc dipped to %u mV (below %u mV floor) — "
                 "brownout precursor\n",
                 addr, (unsigned)u.vitals.vccMin_mV,
                 (unsigned)UNIT_VCC_MIN_FLOOR_MV);
  if (t.onset & UNIT_EVT_JAM)
    SerialPrintf("Unit 0x%02x: jam (stalled move)\n", addr);
  if (t.onset & UNIT_EVT_DRAG)
    SerialPrintf("Unit 0x%02x: steps-to-home drag (excess %u > %u steps)\n",
                 addr, (unsigned)u.extDiag.stepExcessMax,
                 (unsigned)EXT_DIAG_DRAG_EXCESS_STEPS);
  if (t.onset & UNIT_EVT_HALL_ANOMALY)
    SerialPrintf("Unit 0x%02x: hall edges/rev anomaly (%u, expected 1)\n",
                 addr, (unsigned)u.extDiag.hallEdgesLastRev);
  if (t.recovery & UNIT_EVT_STALE)
    SerialPrintf("Unit 0x%02x recovered — back on the bus\n", addr);
  if (t.recovery & UNIT_EVT_HOME_FAILED)
    SerialPrintf("Unit 0x%02x recovered — homed OK\n", addr);
}

// Logs a unit reboot (reset cause + lifetime brownout/watchdog counts) the
// same place #322 logs health transitions (#368). Gated on statusValid so a
// unit whose read failed this tick never fabricates a reboot from stale
// rebootWatch state; the durable edge state lives in busFacts (survives
// across ticks, like healthEventState above).
static void logUnitReboot(const DisplaySnapshot& local, UnitFacts* busFacts,
                          int i) {
  const UnitFacts& u = local.units[i];
  if (!u.statusValid) return;
  const UnitStatus& s = u.status;
  if (!unitRebootDetect(busFacts[i].rebootWatch, s.uptimeSeconds,
                        s.lifetimeBrownoutCount, s.lifetimeWatchdogCount)) {
    return;
  }
  int addr = SFP_I2C_ADDRESS_BASE + i;
  SerialPrintf("Unit 0x%02x rebooted (%s) — brownouts=%u watchdogs=%u\n", addr,
               unitResetCauseName(unitResetCauseDecode(s.mcusrAtBoot)),
               (unsigned)s.lifetimeBrownoutCount,
               (unsigned)s.lifetimeWatchdogCount);
}

// One opportunistic heartbeat read (#310), synthesized by displayTask only on
// an idle tick — display writes / reflash / an explicit Probe always preempt
// (they arrive as commands). Round-robins one unit per tick; skipped entirely
// while a unit may be in its twiboot window (v1 #88: a status read pins the
// bootloader alive).
static void heartbeatTick(DisplaySnapshot& local, UnitFacts* busFacts,
                          int& slot) {
  int width = local.displayWidth;
  if (width <= 0) return;
  if ((int32_t)(twibootRiskUntilMs - millis()) > 0) return;
  int i = slot;
  slot = heartbeatNextSlot(slot, width);
  bool ok = unitBusPollHealthOne(busFacts, i);
  heartbeatApply(busFacts[i], ok, millis(), HEARTBEAT_MISS_THRESHOLD);
  displayApplyUnitFacts(local, busFacts, UNITS_AMOUNT,
                        effectiveWidthOverride());
  logUnitHealthTransition(local, busFacts, i);  // #322
  logUnitReboot(local, busFacts, i);  // #368
  headlessTrack(local);  // #329: idle-tick observation feeds the debounce
  snapshotPublish(local);
}

// --- unit reflash job (#205) ---------------------------------------------------
// The one long-running job in the firmware, run INLINE by displayTask (the
// design's approach A: bus exclusivity stays structural, commands queue
// behind it — except nothing queues, because the reflashActive gate turns
// producers away at their boundaries while this runs).
//
// The job's internal probes deliberately BYPASS settleBeforeProbe(): the
// v1 #88 hazard is probing a twiboot unit *without* flashing it (the
// CHIPINFO query pins the bootloader alive forever); here every pinned
// unit is immediately flashed and exited, and a FAILED unit is left
// pinned on purpose — it must not jump to a torn sketch, and the pin
// keeps it reachable for the retry.

// Which sketch units the pre-flash reboot sweep targets.
enum class ReflashSweep : uint8_t {
  OffBundle,     // web job: outdated + unknown revs (v1 #114 semantics)
  OutdatedOnly,  // boot auto-update: only provably stale revs
};

// True when a job would have work: something sits in twiboot already, or
// the sweep predicate matches a sketch unit. Boot uses this to skip the
// whole job (and its progress churn) on a healthy display.
static bool reflashHasWork(const DisplaySnapshot& snap, ReflashSweep sweep) {
  uint8_t addrs[UNITS_AMOUNT];
  if (reflashCollectFlashTargets(snap.units, UNITS_AMOUNT,
                                 SFP_I2C_ADDRESS_BASE, addrs) > 0) {
    return true;
  }
  int n = sweep == ReflashSweep::OffBundle
              ? reflashCollectRebootTargets(snap.units, UNITS_AMOUNT,
                                            SFP_I2C_ADDRESS_BASE, addrs)
              : reflashCollectOutdatedTargets(snap.units, UNITS_AMOUNT,
                                              SFP_I2C_ADDRESS_BASE, addrs);
  return n > 0;
}

// `onlyAddr` narrows the whole job to one unit (0 = the whole fleet, today's
// behaviour). The #407 campaign flashes a day-0 image that has never run on
// hardware, so the operator wants to convince themselves on unit 1 before
// unit 2 exists (#412).
static void runReflashJob(DisplaySnapshot& local, UnitFacts* busFacts,
                          ReflashSweep sweep, uint8_t onlyAddr) {
  reflashProgressBegin(local.reflash, 0);  // total known after the rescan
  snapshotPublish(local);                  // gate closes here

  // With the gate closed, drain whatever slipped into the queue earlier —
  // it would burst-drain onto a display this job is about to rebuild.
  // Stop survives (it is the cancel; its abort flag is already set — the
  // #204 order rule) but is re-sent only AFTER the drain finishes: a
  // mid-drain re-send would leave commands that sat behind the Stop
  // running ahead of it after a cancelled job (Codex review finding).
  // Multiple Stops collapse into one — supersession is the #204 contract.
  DisplayCommand stale;
  bool sawStop = false;
  DisplayCommand stopCmd;
  while (xQueueReceive(displayQueue, &stale, 0) == pdTRUE) {
    if (stale.opcode == DisplayOpcode::Stop) {
      sawStop = true;
      stopCmd = stale;
      continue;
    }
    SerialPrintln("display: dropped queued command at reflash start: " +
                  describeDisplayCommand(stale));
  }
  if (sawStop) xQueueSend(displayQueue, &stopCmd, 0);

  // Push sweep-matching sketch units into twiboot (v1's
  // enterBootloaderAllDetected), then wait out the watchdog reset +
  // twiboot init before talking to anyone.
  uint8_t addrs[UNITS_AMOUNT];
  int rebooted = 0;
  int sweepCount =
      sweep == ReflashSweep::OffBundle
          ? reflashCollectRebootTargets(local.units, UNITS_AMOUNT,
                                        SFP_I2C_ADDRESS_BASE, addrs)
          : reflashCollectOutdatedTargets(local.units, UNITS_AMOUNT,
                                          SFP_I2C_ADDRESS_BASE, addrs);
  sweepCount = reflashFilterToAddress(addrs, sweepCount, onlyAddr);
  for (int i = 0; i < sweepCount; i++) {
    if (unitBusRebootToBootloader(addrs[i]) == 0) {
      displayInvalidateUnitReads(local, addrs[i]);
      rebooted++;
    }
  }
  if (rebooted > 0) {
    SerialPrintf("reflash: sent %d unit(s) into the bootloader\n", rebooted);
    delay(TWIBOOT_STARTUP_MS);
  }

  // Rescan (inhibit bypassed by design, see block comment) to see who
  // actually sits in twiboot, then plan the flash list from live truth.
  unitBusProbe(busFacts, UNITS_AMOUNT);
  displayApplyUnitFacts(local, busFacts, UNITS_AMOUNT,
                        effectiveWidthOverride());
  uint8_t targets[UNITS_AMOUNT];
  int total = reflashCollectFlashTargets(local.units, UNITS_AMOUNT,
                                         SFP_I2C_ADDRESS_BASE, targets);
  total = reflashFilterToAddress(targets, total, onlyAddr);
  local.reflash.total = (uint8_t)total;
  snapshotPublish(local);
  SerialPrintf("reflash: %d unit(s) to flash\n", total);

  const uint8_t* image = unitFirmwareBin();
  size_t imageLen = unitFirmwareBinLen();
  bool cancelled = false;
  bool halted = false;
  uint8_t consecutiveFailures = 0;
  uint8_t batch[REFLASH_BATCH_SIZE];
  int inBatch = 0;
  for (int k = 0; k < total; k++) {
    wdtFeed();  // #314: I2C page-streaming is the longest displayTask op
    if (unitBusAbortRequested()) {
      cancelled = true;
      break;
    }
    uint8_t addr = targets[k];
    reflashProgressUnitStart(local.reflash, addr);
    snapshotPublish(local);

    UnitFlashResult r = unitBusFlashUnit(addr, image, imageLen);
    if (r == UnitFlashResult::Aborted) {
      reflashProgressUnitResult(local.reflash, false);
      snapshotPublish(local);
      cancelled = true;
      break;
    }
    bool ok = (r == UnitFlashResult::Ok);
    if (!ok) {
      SerialPrintf("reflash: unit 0x%02x failed (%s)\n", addr,
                   unitFlashResultName(r));
      if (consecutiveFailures < 0xFF) consecutiveFailures++;
    } else {
      consecutiveFailures = 0;  // an isolated dead unit must not wedge a sweep
    }
    reflashProgressUnitResult(local.reflash, ok);
    if (ok) {
      // Just-flashed unit runs the sketch again; bump the fact so the
      // batch-idle wait below polls it (the final reprobe rewrites all
      // facts wholesale anyway).
      local.units[addr - SFP_I2C_ADDRESS_BASE].state = 1;
      batch[inBatch++] = addr;
    }
    snapshotPublish(local);

    // v1 #138 brownout throttle: once a batch is full, wait for those
    // units to come back online + finish homing before flashing more.
    if (inBatch >= REFLASH_BATCH_SIZE) {
      reflashProgressSettling(local.reflash);
      snapshotPublish(local);
      unitBusWaitBatchIdle(batch, inBatch, REFLASH_BATCH_SETTLE_MS);
      inBatch = 0;
    }

    // Stop walking the row (#412). Two failures back to back is the signature
    // of an image that cannot land, not of one dead unit — and the old code
    // would have kept going and broken every remaining unit the same way. The
    // trailing settle below still runs, so units already flashed finish homing
    // before we hand the display back.
    if (reflashShouldHalt(consecutiveFailures)) {
      SerialPrintf("reflash: HALTED after %u consecutive failures — "
                   "%d unit(s) left untouched\n",
                   (unsigned)consecutiveFailures, total - (k + 1));
      halted = true;
      break;
    }
  }
  // Trailing partial batch — reached on plan exhaustion AND on both abort
  // exits: the settle is brownout pacing and is never
  // abort-shortened, so even a cancelled job waits out the homing of the
  // units it already flashed before the queued Stop broadcast-homes.
  if (inBatch > 0) {
    reflashProgressSettling(local.reflash);
    snapshotPublish(local);
    unitBusWaitBatchIdle(batch, inBatch, REFLASH_BATCH_SETTLE_MS);
  }

  // Final reprobe + health poll: published topology and fw grades are
  // execution-time truth (a failed/cancelled unit shows as bootloader and
  // stays pinned there — deliberate, see block comment).
  wdtFeed();  // #314: no feed between the trailing settle and boot-home otherwise
  unitBusProbe(busFacts, UNITS_AMOUNT);
  pollHealthWithFreshness(busFacts);
  displayApplyUnitFacts(local, busFacts, UNITS_AMOUNT,
                        effectiveWidthOverride());
  // Staggered boot-home of the just-flashed units (#309): a reflashed unit
  // reboots UNHOMED, so without this the caller's re-show (or the next cluster
  // render) would home every flashed unit at once — the #305 inrush #309
  // exists to prevent. Targets only the still-unhomed units; a cancel leaves
  // the abort flag set so this bails and the queued Stop broadcast-homes.
  wdtFeed();  // #314: boot-home of just-flashed units
  runBootHomeSequence(local, busFacts);
  reflashProgressFinish(local.reflash, cancelled, halted);
  snapshotPublish(local);  // gate reopens here
  SerialPrintf("reflash: %s — %u ok, %u failed of %u%s\n",
               reflashStateName(local.reflash.state),
               (unsigned)local.reflash.done, (unsigned)local.reflash.failed,
               (unsigned)local.reflash.total,
               halted ? " (HALTED — image suspect, remaining units untouched)"
                      : "");
}

// --- opcode executors (#353): one static helper per DisplayCommand opcode —
// displayTaskMain's switch stays pure dispatch. Uniform signature by design;
// helpers that ignore an argument cast it void.

static void execShowText(DisplaySnapshot& local, UnitFacts* busFacts,
                        const DisplayCommand& cmd) {
  (void)busFacts;
  (void)cmd;
  uint8_t letters[UNITS_AMOUNT];
  flapFrameBuild(cmd.text, local.displayWidth, cmd.alignment,
                 letters);
  int errs = unitBusShowFrame(local.units, local.displayWidth,
                              letters, convertSpeedToUnit(cmd.speed));
  // v1's lastShowUnitWriteErrors — the MQTT unitErrors telemetry
  // input (#224).
  local.lastShowWriteErrors = errs > 0 ? (uint8_t)errs : 0;
  // The "intended" side of the displayed==intended check (#264).
  memcpy(local.lastFrameLetters, letters, sizeof(letters));
  local.lastFrameValid = true;
}

static void execProbe(DisplaySnapshot& local, UnitFacts* busFacts,
                      const DisplayCommand& cmd) {
  (void)cmd;
  // Re-scan + health refresh: an address change moves a unit to a
  // slot only a probe can see (v1 #56 semantics). A refresh queued
  // right behind a /unit/reboot must not scan into the twiboot
  // window — wait the risk deadline out first.
  settleBeforeProbe();
  unitBusProbe(busFacts, UNITS_AMOUNT);
  pollHealthWithFreshness(busFacts);
  displayApplyUnitFacts(local, busFacts, UNITS_AMOUNT,
                effectiveWidthOverride());
  headlessTrack(local);  // #329: explicit-probe observation
}

static void execWriteOffset(DisplaySnapshot& local, UnitFacts* busFacts,
                           const DisplayCommand& cmd) {
  (void)busFacts;
  (void)cmd;
  int status = unitBusWriteOffset(cmd.unitAddress, cmd.value);
  if (status == 0) {
    // The only in-place offset mutation — probes own everything else.
    displayApplyOffsetWrite(local, cmd.unitAddress, cmd.value);
  }
  displayApplyMaintResult(
      local, cmd,
      status == 0 ? MaintOutcome::Ok : MaintOutcome::WireFail,
      MaintReason::None);
}

static void execJog(DisplaySnapshot& local, UnitFacts* busFacts,
                   const DisplayCommand& cmd) {
  (void)busFacts;
  (void)cmd;
  int status = unitBusJog(cmd.unitAddress, cmd.value);
  displayApplyMaintResult(
      local, cmd,
      status == 0 ? MaintOutcome::Ok : MaintOutcome::WireFail,
      MaintReason::None);
}

static void execHome(DisplaySnapshot& local, UnitFacts* busFacts,
                    const DisplayCommand& cmd) {
  (void)busFacts;
  (void)cmd;
  int status = unitBusHome(cmd.unitAddress);
  if (status == 0 && local.lastFrameValid) {
    // The unit parks at blank — keep the intended frame truthful so
    // the #264 mismatch check doesn't flag the deliberate home.
    int idx = cmd.unitAddress - SFP_I2C_ADDRESS_BASE;
    if (idx >= 0 && idx < UNITS_AMOUNT) {
      local.lastFrameLetters[idx] = 0;
    }
  }
  displayApplyMaintResult(
      local, cmd,
      status == 0 ? MaintOutcome::Ok : MaintOutcome::WireFail,
      MaintReason::None);
}

static void execIdentify(DisplaySnapshot& local, UnitFacts* busFacts,
                        const DisplayCommand& cmd) {
  (void)busFacts;
  (void)cmd;
  int status = unitBusIdentify(cmd.unitAddress);
  displayApplyMaintResult(
      local, cmd,
      status == 0 ? MaintOutcome::Ok : MaintOutcome::WireFail,
      MaintReason::None);
}

static void execSelfTest(DisplaySnapshot& local, UnitFacts* busFacts,
                        const DisplayCommand& cmd) {
  (void)busFacts;
  (void)cmd;
  // On-demand diagnostic revolution (#265): start, then poll the
  // unit's result until a terminal state. A stale terminal from a
  // PREVIOUS test can still sit in the unit's reply buffer while
  // this one queues behind an in-flight move — only accept a
  // terminal state after RUNNING has been observed.
  SelfTestSlot slot;
  slot.seq = cmd.seq;
  slot.addr = cmd.unitAddress;
  int status = unitBusStartSelfTest(cmd.unitAddress);
  if (status != 0) {
    slot.outcome = SelfTestOutcome::WireFail;
  } else {
    slot.outcome = SelfTestOutcome::Timeout;
    bool sawRunning = false;
    bool everValid = false;
    bool haveBaseline = false;
    UnitSelfTestReading baseline{};
    int badPolls = 0;
    uint32_t start = millis();
    while (millis() - start < SELF_TEST_TIMEOUT_MS) {
      wdtFeed();  // #314: self-test polls the unit until it reports an outcome
      if (unitBusAbortRequested()) {
        slot.outcome = SelfTestOutcome::Aborted;
        break;
      }
      delay(SELF_TEST_POLL_MS);
      UnitSelfTestReading r;
      if (!unitBusReadSelfTest(cmd.unitAddress, r)) {
        if (!everValid && ++badPolls >= SELF_TEST_UNSUPPORTED_POLLS) {
          slot.outcome = SelfTestOutcome::Unsupported;
          break;
        }
        continue;
      }
      everValid = true;
      if (!haveBaseline) {
        // First valid reading = the pre-test buffer content. A later
        // terminal that DIFFERS from it is provably fresh even when
        // every poll of the RUNNING window was lost to bus glitches.
        haveBaseline = true;
        baseline = r;
      }
      if (r.state == 1) {  // running
        if (!sawRunning) {
          // The test provably started — re-arm the window so time the
          // unit spent finishing a prior move doesn't eat the test's
          // own budget.
          sawRunning = true;
          start = millis();
        }
        continue;
      }
      bool freshTerminal =
          sawRunning ||
          (haveBaseline &&
           (r.state != baseline.state ||
            r.stepsPerRev != baseline.stepsPerRev ||
            r.hallWindowSteps != baseline.hallWindowSteps ||
            r.revTimeMs != baseline.revTimeMs ||
            r.reason != baseline.reason));
      if (r.state == 0 || !freshTerminal) continue;  // not started / stale
      // #404: carry the measurements on BOTH paths. A failure preserves
      // whatever it got to measure — a phase-2 failure knows its hall window,
      // and that is the most diagnostic number the unit has.
      slot.stepsPerRev = r.stepsPerRev;
      slot.hallWindowSteps = r.hallWindowSteps;
      slot.revTimeMs = r.revTimeMs;
      slot.unitReason = r.reason;
      slot.outcome = (r.state == 2) ? SelfTestOutcome::Ok
                                    : SelfTestOutcome::UnitFailed;
      break;
    }
  }
  SerialPrintf("display: self-test unit 0x%02x → %s%s%s\n",
               cmd.unitAddress, selfTestOutcomeName(slot.outcome),
               slot.unitReason != SELFTEST_REASON_NONE ? " / " : "",
               slot.unitReason != SELFTEST_REASON_NONE
                   ? selfTestReasonName(slot.unitReason) : "");
  displayApplySelfTestResult(local, slot);
  displayApplyMaintResult(local, cmd,
                          slot.outcome == SelfTestOutcome::Ok
                              ? MaintOutcome::Ok
                              : MaintOutcome::PostconditionFail,
                          MaintReason::None);
}

static void execResetOdometer(DisplaySnapshot& local, UnitFacts* busFacts,
                             const DisplayCommand& cmd) {
  (void)busFacts;
  (void)cmd;
  int status = unitBusResetOdometer(cmd.unitAddress);
  if (status == 0) {
    // Patch the fact in place like a successful offset write —
    // the wear view must not show the stale count until the next
    // probe (#231).
    displayApplyOdometerReset(local, cmd.unitAddress);
  }
  displayApplyMaintResult(
      local, cmd,
      status == 0 ? MaintOutcome::Ok : MaintOutcome::WireFail,
      MaintReason::None);
}

static void execSetGates(DisplaySnapshot& local, UnitFacts* busFacts,
                        const DisplayCommand& cmd) {
  (void)busFacts;
  int status = unitBusSetGates(cmd.unitAddress, (uint8_t)cmd.value);
  if (status == 0) {
    // Verified by the read-back inside unitBusSetGates — patch the fact so
    // /units/health stops reporting the pre-write gates (#409).
    displayApplyGatesWrite(local, cmd.unitAddress, (uint8_t)cmd.value);
  }
  // A unit that refused the bits answers with its old gates, which the
  // read-back grades as a mismatch — the operator sees the refusal instead
  // of an op that claims to have landed.
  displayApplyMaintResult(
      local, cmd,
      status == 0 ? MaintOutcome::Ok
                  : (status == UNIT_BUS_GATES_MISMATCH ||
                     status == UNIT_BUS_GATES_UNVERIFIED)
                        ? MaintOutcome::PostconditionFail
                        : MaintOutcome::WireFail,
      MaintReason::None);
}

static void execRebootToBootloader(DisplaySnapshot& local, UnitFacts* busFacts,
                                  const DisplayCommand& cmd) {
  (void)busFacts;
  (void)cmd;
  // NO follow-up probe (v1 #88 hard rule): the unit sits in twiboot
  // ~1 s and the probe's CHIPINFO query would pin it there forever.
  // Reads for this unit are invalidated until the next probe.
  int status = unitBusRebootToBootloader(cmd.unitAddress);
  if (status == 0) {
    displayInvalidateUnitReads(local, cmd.unitAddress);
    armTwibootRiskWindow();
  }
  displayApplyMaintResult(
      local, cmd,
      status == 0 ? MaintOutcome::Ok : MaintOutcome::WireFail,
      MaintReason::None);
}

static void execSetAddress(DisplaySnapshot& local, UnitFacts* busFacts,
                          const DisplayCommand& cmd) {
  (void)busFacts;
  (void)cmd;
  // Execution-time recheck against LIVE facts: the web handler
  // validated a snapshot copy that the queue delay made stale.
  MaintVerdict verdict = maintValidateSetAddressTarget(
      cmd.value, cmd.unitAddress, local.units, UNITS_AMOUNT);
  if (verdict.httpStatus != 200) {
    displayApplyMaintResult(
        local, cmd, MaintOutcome::ExecValidationFail,
        verdict.httpStatus == 409 ? MaintReason::TargetAddressOccupied
                                  : MaintReason::None);
    return;
  }
  int status = unitBusSetAddress(cmd.unitAddress, (uint8_t)cmd.value);
  if (status != 0) {
    displayApplyMaintResult(local, cmd, MaintOutcome::WireFail,
                            MaintReason::None);
    return;
  }
  // Compound op: settle THROUGH the unit's reboot + twiboot window
  // (probing inside it pins twiboot — v1 #88), then reprobe so the
  // published topology is execution-time truth, not a UI timer race.
  // NOT abort-shortened: the settle is bus safety, not pacing.
  armTwibootRiskWindow();
  settleBeforeProbe();
  unitBusProbe(busFacts, UNITS_AMOUNT);
  pollHealthWithFreshness(busFacts);
  displayApplyUnitFacts(local, busFacts, UNITS_AMOUNT,
                effectiveWidthOverride());
  MaintReason reason = MaintReason::None;
  MaintOutcome outcome = classifySetAddressOutcome(
      local.units, UNITS_AMOUNT, cmd.value, reason);
  displayApplyMaintResult(local, cmd, outcome, reason);
}

static void execClearAddress(DisplaySnapshot& local, UnitFacts* busFacts,
                            const DisplayCommand& cmd) {
  (void)busFacts;
  (void)cmd;
  int countBefore = local.detectedUnitCount;
  int status = unitBusClearAddress(cmd.unitAddress);
  if (status != 0) {
    displayApplyMaintResult(local, cmd, MaintOutcome::WireFail,
                            MaintReason::None);
    return;
  }
  armTwibootRiskWindow();
  settleBeforeProbe();
  unitBusProbe(busFacts, UNITS_AMOUNT);
  pollHealthWithFreshness(busFacts);
  displayApplyUnitFacts(local, busFacts, UNITS_AMOUNT,
                effectiveWidthOverride());
  MaintReason reason = MaintReason::None;
  MaintOutcome outcome = classifyClearAddressOutcome(
      countBefore, local.detectedUnitCount, reason);
  displayApplyMaintResult(local, cmd, outcome, reason);
}

static void execResetUnits(DisplaySnapshot& local, UnitFacts* busFacts,
                          const DisplayCommand& cmd) {
  (void)busFacts;
  (void)cmd;
  // v1 blank-out sequence: a full row of '-', 2 s, a full row of
  // '.' — the wrap-around forces each unit's recalibration — then
  // re-show the text baked at enqueue time.
  uint8_t letters[UNITS_AMOUNT];
  char row[UNITS_AMOUNT + 1];
  int unitSpeed = convertSpeedToUnit(cmd.speed);
  memset(row, '-', local.displayWidth);
  row[local.displayWidth] = '\0';
  flapFrameBuild(row, local.displayWidth, DisplayAlignment::Left,
                 letters);
  unitBusShowFrame(local.units, local.displayWidth, letters,
                   unitSpeed);
  delay(2000);
  memset(row, '.', local.displayWidth);
  row[local.displayWidth] = '\0';
  flapFrameBuild(row, local.displayWidth, DisplayAlignment::Left,
                 letters);
  unitBusShowFrame(local.units, local.displayWidth, letters,
                   unitSpeed);
  flapFrameBuild(cmd.text, local.displayWidth, cmd.alignment,
                 letters);
  unitBusShowFrame(local.units, local.displayWidth, letters,
                   unitSpeed);
  memcpy(local.lastFrameLetters, letters, sizeof(letters));  // #264
  local.lastFrameValid = true;
  displayApplyMaintResult(local, cmd, MaintOutcome::Ok,
                          MaintReason::None);
}

static void execStop(DisplaySnapshot& local, UnitFacts* busFacts,
                    const DisplayCommand& cmd) {
  (void)busFacts;
  (void)cmd;
  // The abort flag (set by the /stop handler at enqueue) already
  // short-circuited every wait ahead of us; now park the display.
  int status = unitBusBroadcastHome();
  if (status == 0) {
    // Every unit parks at blank — the intended frame follows (#264).
    memset(local.lastFrameLetters, 0, sizeof(local.lastFrameLetters));
    local.lastFrameValid = true;
  }
  unitBusClearAbort();
  displayApplyMaintResult(
      local, cmd,
      status == 0 ? MaintOutcome::Ok : MaintOutcome::WireFail,
      MaintReason::None);
}

static void execReflashUnits(DisplaySnapshot& local, UnitFacts* busFacts,
                            const DisplayCommand& cmd) {
  (void)busFacts;
  (void)cmd;
  // The job closes the gate, drains queue stragglers (Stop
  // survives), flashes in batches, and reprobes — see runReflashJob.
  runReflashJob(local, busFacts, ReflashSweep::OffBundle, cmd.unitAddress);

  // Baked re-show: reflashed units homed to blank — put the
  // enqueue-time content back. Skipped on cancel: the queued Stop
  // right behind us broadcast-homes and clears the text anyway.
  if (local.reflash.state != ReflashState::Cancelled) {
    uint8_t letters[UNITS_AMOUNT];
    flapFrameBuild(cmd.text, local.displayWidth, cmd.alignment,
                   letters);
    unitBusShowFrame(local.units, local.displayWidth, letters,
                     convertSpeedToUnit(cmd.speed));
    memcpy(local.currentText, cmd.text, sizeof(local.currentText));
    memcpy(local.lastFrameLetters, letters, sizeof(letters));  // #264
    local.lastFrameValid = true;
  }
  MaintReason reason = MaintReason::None;
  MaintOutcome outcome = classifyReflashOutcome(local.reflash, reason);
  displayApplyMaintResult(local, cmd, outcome, reason);
}

void displayTaskMain(void*) {
  SerialPrintf("displayTask up on core %d\n", xPortGetCoreID());
  DisplaySnapshot local;  // task-private working state; published as copies
  // Static: ~400 B that would otherwise sit on the task stack forever.
  static UnitFacts busFacts[UNITS_AMOUNT];

  unitBusInit();
  // Subscribe BEFORE the boot probe/reflash/boot-home block: those ops carry
  // wdtFeed() calls that are silent no-ops for an unsubscribed task, and a
  // wedged I2C transaction on the cold first scan must still trip the dog.
  if (esp_err_t e = wdtSubscribeSelf(); e != ESP_OK)
    SerialPrintf("wdt: display subscribe -> %s\n", esp_err_to_name(e));
  // Load-bearing pre-probe delay (v1 #88): probing earlier catches units
  // still in twiboot's boot window and the CHIPINFO read pins them there.
  delay(1500);
  unitBusProbe(busFacts, UNITS_AMOUNT);
  pollHealthWithFreshness(busFacts);
  displayApplyUnitFacts(local, busFacts, UNITS_AMOUNT,
                        effectiveWidthOverride());
  headlessTrack(local);  // #329: first (boot) observation
  snapshotPublish(local);
  if (local.detectedUnitCount == 0) {
    if (local.displayWidth == 0) {
      SerialPrintln("display: no units — headless role, display disabled");  // #331
    } else {
      SerialPrintf("display: no units responding — assuming full width %d\n",
                   local.displayWidth);
    }
  } else {
    SerialPrintf("display: probe done, width %d\n", local.displayWidth);
  }
  if (tasksUnitCountOverridePinned()) {
    SerialPrintf("display: width pinned to %d (unit-count override)\n",
                 local.displayWidth);
  }

  // Boot auto-install + auto-update (#205, full v1 parity): flash any unit
  // the probe found sitting in twiboot (the recovery path for a failed or
  // cancelled flash), and push provably-outdated sketch units through the
  // same job. Runs before the command loop, but the WiFi join is already
  // racing on core 0 — the job's reflashActive gate turns away whatever
  // comes up mid-install (web 409s, clock skips).
  // Staggered boot-home (#309): the units boot UNHOMED, so the master
  // orchestrates the homing inrush in bounded batches instead of letting the
  // whole row's steppers spike the shared rail at once (the #305 verify-boot
  // brownout). runReflashJob ends with its own boot-home of the units it
  // flashed, so only home here when no boot reflash ran.
  if (!tasksReflashOnBoot()) {
    // #412: deliberately suppressed for a gated campaign. Say so loudly — a
    // silently-skipped auto-install looks exactly like a healthy display, and
    // this setting persists across reboots.
    SerialPrintln(F("reflash: boot auto-install SUPPRESSED (reflashOnBoot=false)"));
    runBootHomeSequence(local, busFacts);
  } else if (reflashHasWork(local, ReflashSweep::OutdatedOnly)) {
    SerialPrintln(F("reflash: boot auto-install/auto-update starting"));
    runReflashJob(local, busFacts, ReflashSweep::OutdatedOnly, 0);
  } else {
    runBootHomeSequence(local, busFacts);
  }

  DisplayCommand cmd;
  int heartbeatSlot = 0;  // round-robin cursor for the scheduled poll (#310)
  for (;;) {
    wdtFeed();
#ifdef TWDT_HANG_TEST
    // #314 bench: after 20 s of normal running, wedge displayTask forever so
    // the TWDT must reboot within ~30 s. NEVER defined in a shipping build.
    if (millis() > 20000) { for (;;) { /* no wdtFeed() → dog fires */ } }
#endif
    // Timed wait: a real command preempts (display writes / reflash / Probe);
    // an idle timeout synthesizes one opportunistic heartbeat read.
    if (xQueueReceive(displayQueue, &cmd,
                      pdMS_TO_TICKS(HEARTBEAT_TICK_MS)) != pdTRUE) {
      heartbeatTick(local, busFacts, heartbeatSlot);
      continue;
    }
    local.busy = true;
    snapshotPublish(local);
    if (displayApplyCommand(local, cmd, millis())) {
      SerialPrintln("display: " + describeDisplayCommand(cmd));
      switch (cmd.opcode) {
        case DisplayOpcode::ShowText:
          execShowText(local, busFacts, cmd);
          break;
        case DisplayOpcode::Probe:
          execProbe(local, busFacts, cmd);
          break;
        // --- calibration + provisioning (#204). Every op grades a
        // MaintResult; the web layer serves it via /unit/op-result.
        case DisplayOpcode::WriteOffset:
          execWriteOffset(local, busFacts, cmd);
          break;
        case DisplayOpcode::Jog:
          execJog(local, busFacts, cmd);
          break;
        case DisplayOpcode::Home:
          execHome(local, busFacts, cmd);
          break;
        case DisplayOpcode::Identify:
          execIdentify(local, busFacts, cmd);
          break;
        case DisplayOpcode::SelfTest:
          execSelfTest(local, busFacts, cmd);
          break;
        case DisplayOpcode::SetGates:
          execSetGates(local, busFacts, cmd);
          break;
        case DisplayOpcode::ResetOdometer:
          execResetOdometer(local, busFacts, cmd);
          break;
        case DisplayOpcode::RebootToBootloader:
          execRebootToBootloader(local, busFacts, cmd);
          break;
        case DisplayOpcode::SetAddress:
          execSetAddress(local, busFacts, cmd);
          break;
        case DisplayOpcode::ClearAddress:
          execClearAddress(local, busFacts, cmd);
          break;
        case DisplayOpcode::ResetUnits:
          execResetUnits(local, busFacts, cmd);
          break;
        case DisplayOpcode::Stop:
          execStop(local, busFacts, cmd);
          break;
        case DisplayOpcode::ReflashUnits:
          execReflashUnits(local, busFacts, cmd);
          break;
        default:
          break;
      }
    } else {
      SerialPrintln("display: dropped un-executable command");
    }
    local.busy = false;
    snapshotPublish(local);
  }
}
