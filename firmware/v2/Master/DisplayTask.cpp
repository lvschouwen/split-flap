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
#include "BootDumpOp.h"     // the shared boot-section dump
#include "BootUpdateOp.h"   // the shared in-system twiboot update
#include "UnitUpdateJob.h"  // quiet wait + boot sweep around the flash loop
#include "SelfTestPoll.h"   // the shared self-test wait
#include "BootTrace.h"  // #504
#include "BootUpdatePlan.h"  // #499 decision logic
#include "CrashContext.h"  // #504
#include "FlapFrame.h"
#include "HeartbeatPolicy.h"
#include "HelpersSerialHandling.h"
#include "MotionBudget.h"  // motion admission (#505)
#include "Settings.h"  // SETTINGS_DEFAULT_FLAP_SPEED (rescue re-show seed)
#include "TaskWatchdog.h"
#include "TasksInternal.h"
#include "UnitBus.h"
#include "UnitEventLog.h"  // per-unit health transition log decision (#322)
#include "UnitTimings.h"
#include "UnitRescuePolicy.h"  // runtime rescue of lost units (#498)
#include "WebEndpoints.h"
#include "WifiService.h"  // wifiRadioBusy (#505)

// --- core 1: display domain ---------------------------------------------------

// Exclusive owner of I2C/Wire (via UnitBus, #203). Boot: bus up → twiboot
// settle → probe → health poll → publish. Loop: pure transition
// (displayApplyCommand) + the hardware work per opcode. Blocking bus
// operations run right here by design — commands queue behind them, and
// the published busy flag covers the whole execution.
// Probe inhibit after any op that reboots a unit THROUGH its twiboot window
// (v1 #88 hard rule: the probe's CHIPINFO query pins twiboot alive). Owned
// by displayTask exclusively; every runtime probe waits this deadline out —
// including a Probe that was already queued behind a unit restart.
static uint32_t twibootRiskUntilMs = 0;
// A unit was sent through its bootloader outside the reflash job, so its
// probe-time reads (offset, odometer, version) are void. Only a probe reads
// them again; the idle tick runs one once the risk window has passed, so the
// unit is whole again without anyone asking.
static bool probeOwedAfterRiskWindow = false;

static void armTwibootRiskWindow() {
  twibootRiskUntilMs = millis() + UNIT_PROBE_INHIBIT_MS;
}

// Wire speed of the last text frame, so a rescued unit (#498) gets its
// letter back at the speed the wall was last driven at.
static int lastFrameUnitSpeed = convertSpeedToUnit(SETTINGS_DEFAULT_FLAP_SPEED);

// Motion admission (#505). displayTask owns both: the radio-quiet gate that
// UnitBus calls before starting motion, and the sag-adaptive cap it pushes
// into UnitBus after each heartbeat.
static MotionRadioGate radioGate;
static MotionBudgetState motionBudget;

static void waitRadioQuiet() {
  uint32_t holdStart = millis();
  bool logged = false;
  for (;;) {
    motionRadioObserve(radioGate, wifiRadioBusy(), millis());
    if (motionRadioQuiet(radioGate, millis())) return;
    if (unitBusAbortRequested()) return;
    if (motionRadioHoldExpired(holdStart, millis())) {
      SerialPrintln(F("motion: radio still busy after the hold cap — moving anyway"));
      return;
    }
    if (!logged) {
      SerialPrintln(F("motion: holding unit moves while the radio is busy"));
      logged = true;
    }
    wdtFeed();
    delay(100);
  }
}

// Folds unit i's since-boot supply minimum into the budget and restores it
// over quiet time; pushes any change into UnitBus.
static void motionBudgetFold(const UnitFacts& u, int i) {
  uint32_t now = millis();
  motionRadioObserve(radioGate, wifiRadioBusy(), now);
  if (u.vitalsValid &&
      motionBudgetObserveVmin(motionBudget, i, u.vitals.vccMin_mV, now)) {
    SerialPrintf("motion: rail sag %u mV at unit 0x%02x — at most %u unit(s) "
                 "move at once\n",
                 (unsigned)u.vitals.vccMin_mV, SFP_I2C_ADDRESS_BASE + i,
                 (unsigned)motionBudget.cap);
    unitBusSetMotionCap(motionBudget.cap);
  } else if (motionBudgetTick(motionBudget, now)) {
    SerialPrintf("motion: rail quiet — at most %u unit(s) move at once\n",
                 (unsigned)motionBudget.cap);
    unitBusSetMotionCap(motionBudget.cap);
  }
}

static void settleBeforeProbe() {
  int32_t remaining = (int32_t)(twibootRiskUntilMs - millis());
  if (remaining > 0) delay((uint32_t)remaining);
}

// --- heartbeat freshness + batched boot-home (#309/#310) ---------------------

// A full health poll + a freshness stamp for every slot (#310, HeartbeatPolicy).
// Used by boot, the explicit Probe and every post-op reprobe so a refresh
// resets the miss counters and makes the unit facts "age" truthful immediately
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
// come from unitBusWaitBatchIdle; abort (stop) bails between batches.
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
// the unit facts JSON, so a unit going bad (or recovering) never reaches the
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
               unitResetKindName(unitResetFromStatusByte(s.mcusrAtBoot)),
               (unsigned)s.lifetimeBrownoutCount,
               (unsigned)s.lifetimeWatchdogCount);
}

// Shows the row's present frame again: after a unit came back, and after a
// job that left a drum at home (#575). It waits for the row to stand still
// first; a letter command homes an unhomed unit before it turns, and a unit
// already on its letter does not move.
static void reshowLastFrame(DisplaySnapshot& local) {
  if (!local.lastFrameValid) return;
  // Nothing is written while a unit may sit in its bootloader: the scan owed
  // after that window shows the frame.
  if ((int32_t)(twibootRiskUntilMs - millis()) > 0) {
    probeOwedAfterRiskWindow = true;
    return;
  }
  const bool wasBusy = local.busy;
  local.busy = true;
  snapshotPublish(local);
  unitBusShowFrame(local.units, local.displayWidth, local.lastFrameLetters,
                   lastFrameUnitSpeed);
  local.busy = wasBusy;
}

// Runtime rescue of lost units (#498, UnitRescuePolicy.h). Runs from the
// heartbeat tick, so never inside the twiboot risk window (heartbeatTick
// returns before reaching here) and never during a reflash (inline job).
static UnitRescueState rescueStates[UNITS_AMOUNT];

static void rescueTick(DisplaySnapshot& local, UnitFacts* busFacts, int i) {
  UnitRescueState& rs = rescueStates[i];
  int addr = SFP_I2C_ADDRESS_BASE + i;
  // Mirrored every tick: a reprobe rebuilds busFacts, the count lives here.
  busFacts[i].rescueExits = rs.exits;
  local.units[i].rescueExits = rs.exits;
  if (unitRescueObserve(rs, busFacts[i])) {
    SerialPrintf("Unit 0x%02x answering again — re-showing the frame\n", addr);
    reshowLastFrame(local);
    return;
  }
  if (unitHeldRecheckDue(busFacts[i], rs, millis())) {
    rs.lastAttemptMs = millis();
    if (!unitBusIsBootloader(addr)) {
      // Power-cycled or replaced: a rescan finds out what is there now.
      SerialPrintf("Unit 0x%02x no longer held in its bootloader — "
                   "rescanning\n", addr);
      unitBusProbe(busFacts, UNITS_AMOUNT);
      pollHealthWithFreshness(busFacts);
      displayApplyUnitFacts(local, busFacts, UNITS_AMOUNT,
                            effectiveWidthOverride());
      snapshotPublish(local);
    }
    return;
  }
  if (!unitRescueDue(busFacts[i], rs, millis())) return;
  TwibootIdentity bootloader;
  UnitRescueProbe probe = unitBusRescueProbe(addr, bootloader);
  unitRescueNoteAttempt(rs, millis(), probe);
  busFacts[i].rescueExits = rs.exits;
  local.units[i].rescueExits = rs.exits;
  switch (probe) {
    case UnitRescueProbe::Bootloader:
      SerialPrintf("Unit 0x%02x lost — found in twiboot, started its app "
                   "(rescue #%u)\n", addr, (unsigned)rs.exits);
      armTwibootRiskWindow();  // let the sketch boot before the next read
      break;
    case UnitRescueProbe::NoAck:
      SerialPrintf("Unit 0x%02x lost — no ACK (attempt %u)\n", addr,
                   (unsigned)rs.attempts);
      break;
    case UnitRescueProbe::SketchSilent:
      SerialPrintf("Unit 0x%02x lost — ACKs but status reads fail "
                   "(attempt %u)\n", addr, (unsigned)rs.attempts);
      break;
    case UnitRescueProbe::CrashHeld:
      // A bootloader unit from here on: no longer "lost", so no further
      // rescue probes, and the update job flashes it where it sits.
      SerialPrintf("Unit 0x%02x lost — held in its bootloader after %u crash "
                   "resets; left there as a reflash target\n", addr,
                   (unsigned)bootloader.crashCount);
      unitFactsBecomeBootloader(busFacts[i], bootloader);
      unitFactsBecomeBootloader(local.units[i], bootloader);
      snapshotPublish(local);
      break;
  }
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
  if (probeOwedAfterRiskWindow) {
    probeOwedAfterRiskWindow = false;
    unitBusProbe(busFacts, UNITS_AMOUNT);
    pollHealthWithFreshness(busFacts);
    displayApplyUnitFacts(local, busFacts, UNITS_AMOUNT,
                          effectiveWidthOverride());
    // What sent a unit through its bootloader left it unhomed.
    reshowLastFrame(local);
    snapshotPublish(local);
    return;
  }
  int i = slot;
  slot = heartbeatNextSlot(slot, width);
  bool ok = unitBusPollHealthOne(busFacts, i);
  heartbeatApply(busFacts[i], ok, millis(), HEARTBEAT_MISS_THRESHOLD);
  displayApplyUnitFacts(local, busFacts, UNITS_AMOUNT,
                        effectiveWidthOverride());
  logUnitHealthTransition(local, busFacts, i);  // #322
  logUnitReboot(local, busFacts, i);  // #368
  motionBudgetFold(busFacts[i], i);  // #505
  rescueTick(local, busFacts, i);  // #498
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
  ForcedOne,     // operator override: the addressed unit, whatever its rev
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

// In-system twiboot update (#499): the sequence, its timeouts and its grading
// are shared/BootUpdateOp.h; this is displayTask's side of it.
struct BootUpdateHooks {
  DisplaySnapshot& local;
  // The update job re-shows the row once at its end; replaying the frame
  // after every unit would turn drums the job exists to leave alone, and
  // would keep the next unit busy past its idle wait.
  bool reshowAfter = true;

  bool readBootInfo(uint8_t addr, BootUpdateReport& out) {
    wdtFeed();
    return unitBusReadBootInfo(addr, out);
  }
  bool waitIdle(uint8_t addr, uint32_t timeoutMs) {
    wdtFeed();
    bool idle = unitBusWaitBatchIdle(&addr, 1, timeoutMs);
    wdtFeed();
    return idle;
  }
  int sendStage(uint8_t addr, uint8_t stage) {
    SerialPrintf("display: boot-update unit 0x%02x stage %u\n", addr, stage);
    return unitBusBootUpdate(addr, stage);
  }
  int home(uint8_t addr) { return unitBusHome(addr); }
  bool isHomed(uint8_t addr) { return unitBusIsHomed(addr); }
  void unitLeftSketch(uint8_t addr) {
    displayInvalidateUnitReads(local, addr);
    armTwibootRiskWindow();
  }
  void holdProbes() { armTwibootRiskWindow(); }
  void pause(uint32_t ms) {
    wdtFeed();
    delay(ms);
  }
  uint32_t nowMs() { return millis(); }
  void reshow() {
    if (!reshowAfter || !local.lastFrameValid) return;
    unitBusShowFrame(local.units, local.displayWidth, local.lastFrameLetters,
                     lastFrameUnitSpeed);
  }
  void note(BootUpdateStep step, MaintReason why,
            const BootUpdateReport& info) {
    static const char* const kStep[] = {"read info", "plan",    "settle",
                                        "stage 1",   "stage 2", "done"};
    SerialPrintf("display: boot-update %s → %s (state %u result %u)\n",
                 kStep[(uint8_t)step],
                 why == MaintReason::None ? "ok" : maintReasonName(why),
                 info.state, info.lastResult);
  }
};

// Does a unit on the bundled firmware report a boot section that is known
// and not the current one? Boot starts the job on that alone. Narrower than
// the job's own sweep on purpose: an unread verdict must not send a healthy
// display through the job on every boot.
static bool bootSweepHasWork(const DisplaySnapshot& snap) {
  for (int i = 0; i < UNITS_AMOUNT; i++) {
    const UnitFacts& u = snap.units[i];
    if (u.state == 1 && u.fwStatus == 0 &&
        u.bootVerdict == BOOT_INTEGRITY_OUTDATED) {
      return true;
    }
  }
  return false;
}

// `onlyAddr` narrows the whole job to one unit (0 = the whole fleet, today's
// behaviour). The #407 campaign flashes a day-0 image that has never run on
// hardware, so the operator wants to convince themselves on unit 1 before
// unit 2 exists (#412).
static void runReflashJob(DisplaySnapshot& local, UnitFacts* busFacts,
                          ReflashSweep sweep, uint8_t onlyAddr) {
  reflashProgressBegin(local.reflash, 0);  // total known once planned
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

  // Let the row finish what it was doing before the first unit leaves for
  // its bootloader: no drum turning, no rail load, while pages are streamed.
  // Idle, not homed — a home is a full turn per unit and nothing here needs
  // one (UnitUpdateJob.h).
  {
    uint8_t rowUnits[UNITS_AMOUNT];
    int rowCount = unitUpdateCollectSketchUnits(
        local.units, UNITS_AMOUNT, SFP_I2C_ADDRESS_BASE, rowUnits);
    wdtFeed();
    if (rowCount > 0 &&
        !unitBusWaitBatchIdle(rowUnits, rowCount, UNIT_UPDATE_QUIET_MS)) {
      SerialPrintln(F("reflash: row still moving after the quiet wait — "
                      "going ahead"));
    }
    wdtFeed();
  }
  // A Stop during the wait ends the job here, before any unit is touched.
  if (unitBusAbortRequested()) {
    reflashProgressFinish(local.reflash, true, false);
    snapshotPublish(local);  // gate reopens here
    SerialPrintln(F("reflash: cancelled before any unit was touched"));
    return;
  }

  // The flash list: the sweep's sketch units and whoever already sits in
  // twiboot. Nobody is sent into the bootloader here — the loop does that
  // for each unit right before its own pages (reflashEnterUnit).
  uint8_t sweepAddrs[UNITS_AMOUNT];
  int sweepCount = 0;
  switch (sweep) {
    case ReflashSweep::OffBundle:
      sweepCount = reflashCollectRebootTargets(local.units, UNITS_AMOUNT,
                                               SFP_I2C_ADDRESS_BASE,
                                               sweepAddrs);
      break;
    case ReflashSweep::OutdatedOnly:
      sweepCount = reflashCollectOutdatedTargets(local.units, UNITS_AMOUNT,
                                                 SFP_I2C_ADDRESS_BASE,
                                                 sweepAddrs);
      break;
    case ReflashSweep::ForcedOne:
      sweepCount = reflashCollectForcedTarget(local.units, UNITS_AMOUNT,
                                              SFP_I2C_ADDRESS_BASE, onlyAddr,
                                              sweepAddrs);
      break;
  }
  uint8_t targets[UNITS_AMOUNT];
  int total = reflashPlanTargets(local.units, UNITS_AMOUNT,
                                 SFP_I2C_ADDRESS_BASE, sweepAddrs, sweepCount,
                                 targets);
  total = reflashFilterToAddress(targets, total, onlyAddr);
  local.reflash.total = (uint8_t)total;
  snapshotPublish(local);
  SerialPrintf("reflash: %d unit(s) to flash\n", total);

  const uint8_t* image = webUnitFirmwareBin();
  size_t imageLen = webUnitFirmwareBinLen();
  // The loop, its batch throttle and the #412 halt are the shared
  // reflashRunTargets (ReflashPlan.h); these are displayTask's hooks.
  struct JobHooks {
    DisplaySnapshot& local;
    const uint8_t* image;
    size_t imageLen;
    bool stopRequested() {
      wdtFeed();  // #314: I2C page-streaming is the longest displayTask op
      return unitBusAbortRequested();
    }
    bool imageFits() { return twibootImageFits(imageLen); }
    bool inBootloader(uint8_t addr) { return unitBusIsBootloader(addr); }
    int enterBootloader(uint8_t addr) {
      int status = unitBusRebootToBootloader(addr);
      if (status == 0) displayInvalidateUnitReads(local, addr);
      return status;
    }
    void pause(uint32_t ms) { delay(ms); }
    void unitNotEntered(uint8_t addr) {
      SerialPrintf("reflash: unit 0x%02x is not in its bootloader — "
                   "not flashed\n", addr);
    }
    ReflashUnitOutcome flashUnit(uint8_t addr) {
      UnitFlashResult r = unitBusFlashUnit(addr, image, imageLen);
      if (r == UnitFlashResult::Aborted) return ReflashUnitOutcome::Stopped;
      if (r == UnitFlashResult::Ok) return ReflashUnitOutcome::Flashed;
      SerialPrintf("reflash: unit 0x%02x failed (%s)\n", addr,
                   unitFlashResultName(r));
      return ReflashUnitOutcome::Failed;
    }
    // Just-flashed unit runs the sketch again; bump the fact so the batch-idle
    // wait polls it (the final reprobe rewrites all facts wholesale anyway).
    void unitFlashed(uint8_t addr) {
      local.units[addr - SFP_I2C_ADDRESS_BASE].state = 1;
    }
    void progressChanged() { snapshotPublish(local); }
    void settleBatch(const uint8_t* addrs, int n) {
      unitBusWaitBatchIdle(addrs, n, REFLASH_BATCH_SETTLE_MS);
    }
    void runHalted(uint8_t consecutiveFailures, int untouched) {
      SerialPrintf("reflash: HALTED after %u consecutive failures — "
                   "%d unit(s) left untouched\n",
                   (unsigned)consecutiveFailures, untouched);
    }
  };
  JobHooks jobHooks{local, image, imageLen};
  ReflashRunEnd runEnd =
      reflashRunTargets(jobHooks, targets, total, local.reflash);
  bool cancelled = runEnd.cancelled;
  bool halted = runEnd.halted;

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

  // Boot sections last (UnitUpdateJob.h): every unit that could be flashed is
  // back in its sketch and homed, which is the state stage 2 needs — so the
  // update costs no further turn of the drum. Skipped after a cancel or a
  // halt: a job that stopped itself does not start a second kind of write.
  bool bootHalted = false;
  if (!cancelled && !halted) {
    uint8_t bootTargets[UNITS_AMOUNT];
    int bootTotal = unitUpdateCollectBootTargets(
        local.units, UNITS_AMOUNT, SFP_I2C_ADDRESS_BASE, bootTargets);
    bootTotal = reflashFilterToAddress(bootTargets, bootTotal, onlyAddr);
    if (bootTotal > 0) {
      SerialPrintf("reflash: %d boot section(s) to update\n", bootTotal);
      struct SweepHooks {
        DisplaySnapshot& local;
        bool stopRequested() {
          wdtFeed();
          return unitBusAbortRequested();
        }
        MaintGrade bootUpdate(uint8_t addr) {
          BootUpdateHooks hooks{local, false};
          MaintGrade grade = bootUpdateRun(hooks, addr);
          SerialPrintf("reflash: boot section of unit 0x%02x → %s%s%s\n", addr,
                       maintOutcomeName(grade.outcome),
                       grade.reason != MaintReason::None ? " / " : "",
                       maintReasonName(grade.reason));
          return grade;
        }
        void progressChanged() { snapshotPublish(local); }
        void sweepHalted(uint8_t consecutiveFailures, int untouched) {
          SerialPrintf("reflash: boot sweep HALTED after %u consecutive "
                       "failures — %d unit(s) left untouched\n",
                       (unsigned)consecutiveFailures, untouched);
        }
      };
      SweepHooks sweepHooks{local};
      BootSweepEnd sweepEnd = unitUpdateRunBootSweep(
          sweepHooks, bootTargets, bootTotal, local.reflash);
      cancelled = sweepEnd.cancelled;
      bootHalted = sweepEnd.halted;
      // The masters judge a boot section on their health poll: publish the
      // new verdicts with the result, not minutes later. After the twiboot
      // window the last unit armed — a status read inside it can pin a unit
      // in its bootloader.
      wdtFeed();
      settleBeforeProbe();
      wdtFeed();
      pollHealthWithFreshness(busFacts);
      displayApplyUnitFacts(local, busFacts, UNITS_AMOUNT,
                            effectiveWidthOverride());
    }
  }

  reflashProgressFinish(local.reflash, cancelled, halted || bootHalted);
  snapshotPublish(local);  // gate reopens here
  SerialPrintf("reflash: %s — %u ok, %u failed of %u; boot sections %u "
               "updated, %u failed%s\n",
               reflashStateName(local.reflash.state),
               (unsigned)local.reflash.done, (unsigned)local.reflash.failed,
               (unsigned)local.reflash.total,
               (unsigned)local.reflash.bootDone,
               (unsigned)local.reflash.bootFailed,
               (halted || bootHalted)
                   ? " (HALTED — image suspect, remaining units untouched)"
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
  lastFrameUnitSpeed = convertSpeedToUnit(cmd.speed);
  int errs = unitBusShowFrame(local.units, local.displayWidth,
                              letters, lastFrameUnitSpeed);
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
  // right behind a unit restart must not scan into the twiboot
  // window — wait the risk deadline out first.
  settleBeforeProbe();
  probeOwedAfterRiskWindow = false;
  unitBusProbe(busFacts, UNITS_AMOUNT);
  pollHealthWithFreshness(busFacts);
  displayApplyUnitFacts(local, busFacts, UNITS_AMOUNT,
                effectiveWidthOverride());
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
  displayApplyMaintResult(local, cmd, maintGradeWire(status));
}

static void execJog(DisplaySnapshot& local, UnitFacts* busFacts,
                   const DisplayCommand& cmd) {
  (void)busFacts;
  (void)cmd;
  int status = unitBusJog(cmd.unitAddress, cmd.value);
  displayApplyMaintResult(local, cmd, maintGradeWire(status));
}

static void execHome(DisplaySnapshot& local, UnitFacts* busFacts,
                    const DisplayCommand& cmd) {
  (void)busFacts;
  (void)cmd;
  int status = unitBusHome(cmd.unitAddress);
  // The unit parks at blank: back to the flap its row is showing.
  if (status == 0) reshowLastFrame(local);
  displayApplyMaintResult(local, cmd, maintGradeWire(status));
}

static void execIdentify(DisplaySnapshot& local, UnitFacts* busFacts,
                        const DisplayCommand& cmd) {
  (void)busFacts;
  (void)cmd;
  int status = unitBusIdentify(cmd.unitAddress);
  displayApplyMaintResult(local, cmd, maintGradeWire(status));
}

static void execSelfTest(DisplaySnapshot& local, UnitFacts* busFacts,
                        const DisplayCommand& cmd) {
  (void)busFacts;
  // On-demand diagnostic revolution (#265): start, then poll the unit until
  // SelfTestPoll.h calls the result — the worst-case displayTask block is
  // ~2x SELF_TEST_TIMEOUT_MS.
  SelfTestSlot slot;
  slot.seq = cmd.seq;
  slot.addr = cmd.unitAddress;
  if (unitBusStartSelfTest(cmd.unitAddress) != 0) {
    slot.outcome = SelfTestOutcome::WireFail;
  } else {
    SelfTestPoll poll;
    selfTestPollBegin(poll, millis());
    while (slot.outcome == SelfTestOutcome::Pending) {
      wdtFeed();  // #314: self-test polls the unit until it reports an outcome
      if (unitBusAbortRequested()) {
        slot.outcome = SelfTestOutcome::Aborted;
        break;
      }
      delay(SELF_TEST_POLL_MS);
      UnitSelfTestReading r;
      bool readOk = unitBusReadSelfTest(cmd.unitAddress, r);
      slot.outcome = selfTestPollObserve(poll, readOk, r, millis(), slot);
    }
  }
  SerialPrintf("display: self-test unit 0x%02x → %s%s%s\n",
               cmd.unitAddress, selfTestOutcomeName(slot.outcome),
               slot.unitReason != SELFTEST_REASON_NONE ? " / " : "",
               slot.unitReason != SELFTEST_REASON_NONE
                   ? selfTestReasonName(slot.unitReason) : "");
  if (selfTestMovedTheDrum(slot.outcome)) reshowLastFrame(local);
  displayApplySelfTestResult(local, slot);
  displayApplyMaintResult(
      local, cmd, maintGradeObserved(slot.outcome == SelfTestOutcome::Ok));
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
  displayApplyMaintResult(local, cmd, maintGradeWire(status));
}

static void execSetGates(DisplaySnapshot& local, UnitFacts* busFacts,
                        const DisplayCommand& cmd) {
  (void)busFacts;
  int status = unitBusSetGates(cmd.unitAddress, (uint8_t)cmd.value);
  if (status == 0) {
    // Verified by the read-back inside unitBusSetGates — patch the fact so
    // the unit facts stop reporting the pre-write gates (#409).
    displayApplyGatesWrite(local, cmd.unitAddress, (uint8_t)cmd.value);
  }
  // A unit that refused the bits answers with its old gates, which the
  // read-back grades as a mismatch — the operator sees the refusal instead
  // of an op that claims to have landed.
  displayApplyMaintResult(local, cmd, maintGradeGates(status));
}

// --- boot-section dump (#511) --------------------------------------------------
// The dumped bytes stay out of the snapshot (1 KB copied on every read). The
// store is written by displayTask and copied out by the web handler under a
// spinlock; the seq ties a copy to the result slot it belongs to.
static uint8_t bootDumpBytes[BOOT_SECTION_LEN];
static uint32_t bootDumpBytesSeq = 0;
static portMUX_TYPE bootDumpMux = portMUX_INITIALIZER_UNLOCKED;

bool displayBootDumpCopy(uint32_t seq, uint8_t* out) {
  taskENTER_CRITICAL(&bootDumpMux);
  bool held = seq != 0 && seq == bootDumpBytesSeq;
  if (held) memcpy(out, bootDumpBytes, BOOT_SECTION_LEN);
  taskEXIT_CRITICAL(&bootDumpMux);
  return held;
}

// displayTask's hooks into the shared boot-section dump (BootDumpOp.h).
struct BootDumpHooks {
  DisplaySnapshot& local;
  int enterBootloader(uint8_t addr) { return unitBusRebootToBootloader(addr); }
  void unitLeftSketch(uint8_t addr) { displayInvalidateUnitReads(local, addr); }
  void holdProbes() { armTwibootRiskWindow(); }
  void pause(uint32_t ms) {
    wdtFeed();
    delay(ms);
  }
  UnitBootReadResult readBootSection(uint8_t addr, uint8_t* out) {
    return unitBusReadBootSection(addr, out);
  }
  bool waitIdle(uint8_t addr, uint32_t timeoutMs) {
    return unitBusWaitBatchIdle(&addr, 1, timeoutMs);
  }
  int home(uint8_t addr) { return unitBusHome(addr); }
  void reshow() {
    if (!local.lastFrameValid) return;
    unitBusShowFrame(local.units, local.displayWidth, local.lastFrameLetters,
                     lastFrameUnitSpeed);
  }
};

static void execBootDump(DisplaySnapshot& local, UnitFacts* busFacts,
                         const DisplayCommand& cmd) {
  (void)busFacts;
  static uint8_t scratch[BOOT_SECTION_LEN];  // displayTask-only
  BootDumpSlot slot;
  slot.seq = cmd.seq;
  slot.addr = cmd.unitAddress;
  BootDumpHooks hooks{local};
  slot.outcome = bootDumpRun(hooks, cmd.unitAddress, scratch);
  if (slot.outcome == BootDumpOutcome::Ok) {
    slot.crc32 = bootDumpCrc32(scratch, BOOT_SECTION_LEN);
    taskENTER_CRITICAL(&bootDumpMux);
    memcpy(bootDumpBytes, scratch, BOOT_SECTION_LEN);
    bootDumpBytesSeq = cmd.seq;
    taskEXIT_CRITICAL(&bootDumpMux);
  }
  SerialPrintf("display: boot-section dump unit 0x%02x → %s (crc32 %08lx)\n",
               cmd.unitAddress, bootDumpOutcomeName(slot.outcome),
               (unsigned long)slot.crc32);
  displayApplyBootDumpResult(local, slot);
  probeOwedAfterRiskWindow = true;  // the dump restarted the unit
  displayApplyMaintResult(
      local, cmd, maintGradeObserved(slot.outcome == BootDumpOutcome::Ok));
}

// Read-only boot report (#499): one GET_BOOT_INFO exchange, no restart, no
// write. A unit on firmware predating the opcode answers its one-byte status
// fallback, which the report's checksum and range checks reject — that reads
// as a failed read here, not as a report.
static void execBootInfo(DisplaySnapshot& local, const DisplayCommand& cmd) {
  BootInfoSlot slot;
  slot.seq = cmd.seq;
  slot.addr = cmd.unitAddress;
  slot.ok = unitBusReadBootInfo(cmd.unitAddress, slot.report);
  slot.done = true;
  if (slot.ok) {
    SerialPrintf("display: boot-info unit 0x%02x → %s crc32 %08lx lock %02x\n",
                 cmd.unitAddress, bootInfoStateName(slot.report.state),
                 (unsigned long)slot.report.bootCrc32,
                 (unsigned)slot.report.lockByte);
  } else {
    SerialPrintf("display: boot-info unit 0x%02x → read fail\n",
                 cmd.unitAddress);
  }
  displayApplyBootInfoResult(local, slot);
  displayApplyMaintResult(
      local, cmd, maintGradeObserved(slot.ok, MaintReason::BootInfoReadFail));
}

static void execBootUpdate(DisplaySnapshot& local, UnitFacts* busFacts,
                           const DisplayCommand& cmd) {
  (void)busFacts;
  BootUpdateHooks hooks{local};
  MaintGrade grade = bootUpdateRun(hooks, cmd.unitAddress);
  SerialPrintf("display: boot-update unit 0x%02x → %s%s%s\n", cmd.unitAddress,
               maintOutcomeName(grade.outcome),
               grade.reason != MaintReason::None ? " / " : "",
               maintReasonName(grade.reason));
  displayApplyMaintResult(local, cmd, grade);
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
    probeOwedAfterRiskWindow = true;
  }
  displayApplyMaintResult(local, cmd, maintGradeWire(status));
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
  // The abort flag (set by the stop action at enqueue) already
  // short-circuited every wait ahead of us. Clear it BEFORE parking: the
  // park is a budgeted blank frame (#505) whose admission waits must run —
  // a broadcast HOME started every stepper at once.
  unitBusClearAbort();
  uint8_t blanks[UNITS_AMOUNT] = {0};
  int errs = unitBusShowFrame(local.units, local.displayWidth, blanks,
                              lastFrameUnitSpeed);
  // Every unit parks at blank — the intended frame follows (#264).
  memset(local.lastFrameLetters, 0, sizeof(local.lastFrameLetters));
  local.lastFrameValid = true;
  displayApplyMaintResult(local, cmd, maintGradeWire(errs));
}

static void execReflashUnits(DisplaySnapshot& local, UnitFacts* busFacts,
                            const DisplayCommand& cmd) {
  (void)busFacts;
  (void)cmd;
  // The job closes the gate, drains queue stragglers (Stop
  // survives), flashes in batches, and reprobes — see runReflashJob.
  runReflashJob(local, busFacts,
                cmd.value != 0 ? ReflashSweep::ForcedOne
                               : ReflashSweep::OffBundle,
                cmd.unitAddress);

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
  unitBusSetMotionGate(waitRadioQuiet);  // #505: before boot-home can move
  unitBusSetMotionCap(motionBudget.cap);
  // Subscribe BEFORE the boot probe/reflash/boot-home block: those ops carry
  // wdtFeed() calls that are silent no-ops for an unsubscribed task, and a
  // wedged I2C transaction on the cold first scan must still trip the dog.
  if (esp_err_t e = wdtSubscribeSelf(); e != ESP_OK)
    SerialPrintf("wdt: display subscribe -> %s\n", esp_err_to_name(e));
  delay(UNIT_BOOT_PREPROBE_DELAY_MS);  // load-bearing, see UnitTimings.h
  unitBusProbe(busFacts, UNITS_AMOUNT);
  pollHealthWithFreshness(busFacts);
  displayApplyUnitFacts(local, busFacts, UNITS_AMOUNT,
                        effectiveWidthOverride());
  snapshotPublish(local);
  bootTraceMarkStage(BOOT_STAGE_UNITS);  // #504
  if (local.detectedUnitCount == 0) {
    SerialPrintf("display: no units responding — assuming full width %d\n",
                 local.displayWidth);
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
  } else if (reflashHasWork(local, ReflashSweep::OutdatedOnly) ||
             bootSweepHasWork(local)) {
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
    crashCtxMark(CRASH_SLOT_DISPLAY, CRASH_ACT_IDLE);  // #504
    // Timed wait: a real command preempts (display writes / reflash / Probe);
    // an idle timeout synthesizes one opportunistic heartbeat read.
    if (xQueueReceive(displayQueue, &cmd,
                      pdMS_TO_TICKS(HEARTBEAT_TICK_MS)) != pdTRUE) {
      heartbeatTick(local, busFacts, heartbeatSlot);
      continue;
    }
    local.busy = true;
    snapshotPublish(local);
    if (displayApplyCommand(local, cmd)) {
      SerialPrintln("display: " + describeDisplayCommand(cmd));
      switch (cmd.opcode) {
        case DisplayOpcode::ShowText:
          execShowText(local, busFacts, cmd);
          break;
        case DisplayOpcode::Probe:
          execProbe(local, busFacts, cmd);
          break;
        // --- calibration + provisioning (#204). Every op grades a
        // MaintResult; the web layer serves it as the job's result.
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
        case DisplayOpcode::BootDump:
          execBootDump(local, busFacts, cmd);
          break;
        case DisplayOpcode::BootUpdate:
          execBootUpdate(local, busFacts, cmd);
          break;
        case DisplayOpcode::BootInfo:
          execBootInfo(local, cmd);
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
