// FollowerBusFlash.cpp — runtime rescue of lost units, the twiboot unit
// flash, boot-home, and the reflash / boot-update / boot-dump jobs. The only
// file of the family that includes UnitAssets.h. Shared state is
// FollowerBusInternal.h.

#include "FollowerBus.h"
#include "FollowerBusInternal.h"

#include "BootHomePlan.h"   // pure batched boot-home target selection (#309)
#include "BuildVersion.h"  // GIT_REV + BUNDLED_UNIT_REV (build_assets.py)
#include "FollowerBusRecovery.h"
#include "FollowerConfig.h"
#include "SplitFlapProtocol.h"
#include "TwibootFlash.h"
#include "UnitBusCore.h"
#include "UnitBusTwiboot.h"  // rescue probe, boot-section read, unit flash
#include "UnitAssets.h"  // UNIT_FIRMWARE_BIN (build_assets.py)
#include "UnitProtocolHelpers.h"
#include "UnitTimings.h"
#include "UnitRescuePolicy.h"  // runtime rescue of lost units (#498)
#include "BootUpdateOp.h"     // the shared in-system twiboot update (#499)
#include "UnitUpdateJob.h"    // quiet wait + boot sweep around the flash loop
#include "BootDumpOp.h"       // the shared boot-section dump (#511)

// --- twiboot flash (v1 ServiceFirmwareFunctions port) --------------------------------

static String flashError;

#if SERIAL_ENABLE == false
// Runtime rescue of lost units (#498, UnitRescuePolicy.h). Reached only from
// the heartbeat tick: never inside the probe-inhibit window, never during a
// reflash. A dead bus is the row-wide recovery's job, not a per-unit probe's.
static UnitRescueState rescueStates[UNITS_AMOUNT];

void rescueTick(int i) {
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
  } else if (probe == UnitRescueProbe::ExitRefused) {
    SerialPrint(F(": lost — in twiboot, did not take the order to start its "
                  "app (attempt "));
    SerialPrint(rs.attempts);
    busArmProbeInhibit(millis() + UNIT_PROBE_INHIBIT_MS);  // still in twiboot
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
  void waitingForReset(uint8_t addr) {
    SerialPrint(F("Unit "));
    SerialPrint(addr);
    SerialPrintln(F(": does not enter its bootloader — waiting for a reset "
                    "of the unit by hand"));
  }
  void unitRetried(uint8_t addr, int attempt) {
    SerialPrint(F("Unit "));
    SerialPrint(addr);
    SerialPrint(F(": still in its bootloader — flashing it again, attempt "));
    SerialPrintln(attempt);
  }
  ReflashUnitOutcome flashUnit(uint8_t addr) {
    return flashUnitFromProgmem(addr) ? ReflashUnitOutcome::Flashed
                                      : ReflashUnitOutcome::Failed;
  }
  void unitFlashed(uint8_t addr) {
    UnitFacts& u = unitFacts[addr - SFP_I2C_ADDRESS_BASE];
    u.state = 1;
    strncpy(u.rev, BUNDLED_UNIT_REV, 8);
    u.rev[8] = '\0';
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

static bool flashBootloaderUnits(const uint8_t* targets, int count,
                                 uint32_t handResetWaitMs = 0) {
  ReflashLoopHooks hooks;
  ReflashRunEnd end = reflashRunTargets(hooks, targets, count, reflashProgress,
                                        handResetWaitMs);
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
// The update job's own poll. The job holds the gate, so the public poll
// stands down for it; this one waits out the twiboot window first — a status
// read inside it can pin a unit in its bootloader.
static void jobPollHealth() {
  int32_t remaining = (int32_t)(busProbeInhibitedUntilMs() - millis());
  if (remaining > 0) delay((uint32_t)remaining);
  pollHealthAll();
}

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
      force ? reflashCollectForcedTarget(UNITS_AMOUNT, SFP_I2C_ADDRESS_BASE,
                                         onlyAddr, sweep)
            : reflashCollectRebootTargets(unitFacts, UNITS_AMOUNT,
                                          SFP_I2C_ADDRESS_BASE, sweep);
  uint8_t flashTargets[UNITS_AMOUNT];
  int n = reflashPlanTargets(unitFacts, UNITS_AMOUNT, SFP_I2C_ADDRESS_BASE,
                             sweep, sweepCount, flashTargets);
  n = reflashFilterToAddress(flashTargets, n, onlyAddr);
  reflashProgress.total = (uint8_t)n;
  const uint32_t handResetWaitMs = force ? REFLASH_HAND_RESET_WAIT_MS : 0;
  bool halted = flashBootloaderUnits(flashTargets, n, handResetWaitMs);
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
