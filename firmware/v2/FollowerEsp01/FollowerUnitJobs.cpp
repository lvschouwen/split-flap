// FollowerUnitJobs.cpp — the staged unit job and its drain. Contract in
// FollowerUnitJobs.h.

#include "FollowerUnitJobs.h"

#include <Arduino.h>

#include "BootDump.h"
#include "FollowerBus.h"
#include "FollowerConfig.h"
#include "FollowerMem.h"
#include "FollowerRescue.h"
#include "SelfTestPoll.h"
#include "UnitTimings.h"
#include "WearPolicy.h"

static volatile bool unitHealthRefreshPending = false;

struct StagedOp {
  volatile bool pending = false;
  uint32_t seq = 0;
  FollowerOpKind kind = FollowerOpKind::None;
  uint8_t addr = 0;
  long arg = 0;
};
static StagedOp stagedOp;
static MaintResult opResult;
static SelfTestSlot selfTestSlot;
static BootInfoSlot bootInfoSlot;  // #499: last read-only boot report
static BootDumpSlot bootDumpSlot;  // #522: last boot-section dump result
// The raw 1 KB section is a buffer of FollowerMem.h, held only around a dump:
// claimed when one is staged, given back once the result has had time to go
// up the link or a unit update needs the room.
static uint8_t* bootDumpBytes = nullptr;
static uint32_t bootDumpBytesSeq = 0;  // seq that wrote bootDumpBytes
static uint32_t bootDumpBytesAtMs = 0;  // claimed or last written
#define BOOT_DUMP_KEEP_MS (5UL * 60UL * 1000UL)
static uint32_t maintSeqCounter = 0;
// A job whose outcome is read off the rescan it waits for (seq 0 = none): a
// Probe is done once the scan has run, an address job is judged by what the
// scan finds (MaintenancePolicy.h).
struct AwaitingScan {
  uint32_t seq = 0;
  FollowerOpKind kind = FollowerOpKind::None;
  uint8_t target = 0;   // SetAddress: where the unit must answer
  int countBefore = 0;  // ClearAddress: the units that answered before
};
static AwaitingScan awaitingScan;

// Self-test poll state (the unit measures ~2 revolutions; we poll its
// GET_SELF_TEST until it stops reporting "running").
static bool selfTestPolling = false;
static SelfTestPoll selfTestPoll;
static uint32_t selfTestPollLastMs = 0;

static void releaseBootDumpBytes() {
  if (bootDumpBytes == nullptr) return;
  followerBufFree(bootDumpBytes);
  bootDumpBytes = nullptr;
  bootDumpBytesSeq = 0;
}

// One job at a time: the staged slot, a self-test being waited on, a queued
// rescan and a running unit update all hold the row.
static bool opSlotBusy() {
  return stagedOp.pending || selfTestPolling || awaitingScan.seq != 0 ||
         reflashInProgress(reflashProgress);
}

UnitOpStaged unitOpStage(FollowerOpKind kind, uint8_t addr, long arg,
                         uint32_t& seq) {
  // Rescue mode never touches the bus.
  if (rescueActive()) return UnitOpStaged::Rescue;
  if (opSlotBusy()) return UnitOpStaged::Busy;
  if (kind == FollowerOpKind::BootDump) {
    // Claimed only when the job will be staged: a refused one must not
    // leave 1 KB held through the unit update that made the slot busy.
    if (bootDumpBytes == nullptr) {
      bootDumpBytes = (uint8_t*)followerBufAlloc(BOOT_SECTION_LEN);
      if (bootDumpBytes == nullptr) return UnitOpStaged::NoMemory;
    }
    bootDumpBytesAtMs = millis();
  }
  stagedOp.seq = ++maintSeqCounter;
  stagedOp.kind = kind;
  stagedOp.addr = addr;
  stagedOp.arg = arg;
  stagedOp.pending = true;
  seq = stagedOp.seq;
  return UnitOpStaged::Yes;
}

bool unitOpsBusy() { return opSlotBusy(); }
const MaintResult& unitOpResult() { return opResult; }
const SelfTestSlot& unitOpSelfTest() { return selfTestSlot; }
const BootInfoSlot& unitOpBootInfo() { return bootInfoSlot; }
const uint8_t* unitOpBootDumpBytes(uint32_t seq) {
  return bootDumpBytesSeq == seq ? bootDumpBytes : nullptr;
}

size_t unitsHealthJson(char* buf, size_t cap) {
  int faulty = computeFaultyUnitCount(unitFacts, UNITS_AMOUNT);
  size_t n = buildUnitHealthJson(buf, cap, unitFacts, displayWidth, faulty,
                                 SFP_I2C_ADDRESS_BASE, millis());
  if (n == 0 || n >= cap) {
    n = (size_t)snprintf(buf, cap, "{\"width\":%d,\"faulty\":%d,\"units\":[]}",
                         displayWidth, faulty);
  }
  // Wear and unit-update progress, spliced in behind the units.
  WearAssessment wear;
  assessWear(unitFacts, UNITS_AMOUNT, wear);
  char wearJson[96];
  size_t wearLen = buildWearJson(wear, wearJson, sizeof(wearJson));
  if (n > 0 && wearLen < sizeof(wearJson) && n + wearLen + 2 < cap) {
    n += (size_t)snprintf(buf + n - 1, cap - n + 1, ",%s}", wearJson) - 1;
  }
  char reflashJson[REFLASH_JSON_CAP];
  buildReflashJson(reflashJson, sizeof(reflashJson), reflashProgress);
  if (n > 0 && n + strlen(reflashJson) + 13 < cap) {
    n += (size_t)snprintf(buf + n - 1, cap - n + 1, ",\"reflash\":%s}",
                          reflashJson) - 1;
  }
  return n;
}

// Stamps the one result slot, graded by the shared MaintenancePolicy.h rules.
static void stampOpResult(uint32_t seq, MaintGrade grade) {
  opResult.seq = seq;
  opResult.outcome = grade.outcome;
  opResult.reason = grade.reason;
}

static void executeStagedOp() {
  StagedOp op = stagedOp;  // copy, then release the slot at the end
  MaintGrade grade = maintGradeWire(-1);
  switch (op.kind) {
    case FollowerOpKind::WriteOffset:
      grade = maintGradeWire(busWriteOffset(op.addr, (int16_t)op.arg));
      // Patch the probe-time fact in place: the unit facts carry the new
      // offset without a reprobe.
      if (grade.outcome == MaintOutcome::Ok) {
        unitFactsApplyOffsetWrite(unitFacts[op.addr - SFP_I2C_ADDRESS_BASE],
                                  (int16_t)op.arg);
      }
      break;
    case FollowerOpKind::Jog:
      grade = maintGradeWire(busJog(op.addr, (int)op.arg));
      break;
    case FollowerOpKind::Home:
      grade = maintGradeWire(busHome(op.addr));
      break;
    case FollowerOpKind::Identify:
      grade = maintGradeWire(busIdentify(op.addr));
      break;
    case FollowerOpKind::ResetOdometer:
      grade = maintGradeWire(busResetOdometer(op.addr));
      if (grade.outcome == MaintOutcome::Ok) {
        unitFactsApplyOdometerReset(unitFacts[op.addr - SFP_I2C_ADDRESS_BASE]);
      }
      break;
    case FollowerOpKind::SetGates:
      // busSetGates verifies with a read-back, so a unit that refused the
      // bits grades as a failure here rather than a phantom success.
      grade = maintGradeGates(busSetGates(op.addr, (uint8_t)op.arg));
      if (grade.outcome == MaintOutcome::Ok) {
        unitFactsApplyGatesWrite(unitFacts[op.addr - SFP_I2C_ADDRESS_BASE],
                                 (uint8_t)op.arg);
      }
      break;
    case FollowerOpKind::ReflashUnit:
      releaseBootDumpBytes();  // the flash is this board's heap low-water mark
      // Blocks loop() for the whole update; the slot stays claimed, so every
      // other unit job is refused meanwhile.
      busRunReflashJob(op.addr, op.arg != 0);
      grade = busLastReflashGrade();
      break;
    case FollowerOpKind::RebootToBootloader:
      grade = maintGradeWire(busRebootToBootloader(op.addr));
      if (grade.outcome == MaintOutcome::Ok) busInvalidateUnitReads(op.addr);
      // The unit sits in twiboot for ~1 s — keep every runtime probe out
      // of that window (v1 #88). Armed on a NACK too: it does not prove the
      // unit stayed in its sketch.
      busArmProbeInhibit(millis() + UNIT_PROBE_INHIBIT_MS);
      // Only a probe re-reads the offset; queue one for once the inhibit
      // has run out, or the unit's reads stay invalid until someone asks.
      unitHealthRefreshPending = true;
      break;
    case FollowerOpKind::SelfTest:
      selfTestSlot = SelfTestSlot{};
      selfTestSlot.seq = op.seq;
      selfTestSlot.addr = op.addr;
      if (busStartSelfTest(op.addr) == 0) {
        // The op result follows the test: it is stamped by pollSelfTest once
        // the unit reports, and reads pending until then.
        selfTestPolling = true;
        selfTestPollLastMs = millis();
        selfTestPollBegin(selfTestPoll, selfTestPollLastMs);
        stagedOp.pending = false;
        return;
      }
      selfTestSlot.outcome = SelfTestOutcome::WireFail;
      grade = maintGradeObserved(false);
      break;
    case FollowerOpKind::BootUpdate:
      busRunBootUpdate(op.seq, op.addr, opResult);
      unitHealthRefreshPending = true;  // its reads were invalidated
      stagedOp.pending = false;
      return;
    case FollowerOpKind::BootDump:
      busRunBootDump(op.seq, op.addr, bootDumpSlot, bootDumpBytes);
      bootDumpBytesSeq = (bootDumpSlot.outcome == BootDumpOutcome::Ok)
                             ? op.seq : 0;
      bootDumpBytesAtMs = millis();
      grade = maintGradeObserved(bootDumpSlot.outcome == BootDumpOutcome::Ok);
      unitHealthRefreshPending = true;  // its reads were invalidated
      break;
    case FollowerOpKind::Probe:
      // Runs with the health refresh below, once any twiboot window is over.
      unitHealthRefreshPending = true;
      awaitingScan = AwaitingScan{};
      awaitingScan.seq = op.seq;
      awaitingScan.kind = op.kind;
      stagedOp.pending = false;
      return;
    case FollowerOpKind::SetAddress:
    case FollowerOpKind::ClearAddress: {
      const bool set = op.kind == FollowerOpKind::SetAddress;
      // Judged again here: the plan was made a loop pass ago.
      if (set && maintValidateSetAddressTarget(op.arg, op.addr, unitFacts, UNITS_AMOUNT)
                         .httpStatus != 200) {
        grade = {MaintOutcome::ExecValidationFail, MaintReason::TargetAddressOccupied};
        break;
      }
      const int countBefore = detectedUnitCount;
      const int status = set ? busSetAddress(op.addr, (uint8_t)op.arg) : busClearAddress(op.addr);
      if (status != 0) {
        // A lost ACK does not prove the unit stayed in its sketch: keep the
        // probes out of the window all the same, and look afterwards.
        busArmProbeInhibit(millis() + UNIT_PROBE_INHIBIT_MS);
        unitHealthRefreshPending = true;
        grade = maintGradeWire(status);
        break;
      }
      // The unit restarts through its bootloader: no probe inside that
      // window (v1 #88), and the scan after it says how the job ended.
      busInvalidateUnitReads(op.addr);
      busArmProbeInhibit(millis() + UNIT_PROBE_INHIBIT_MS);
      unitHealthRefreshPending = true;
      awaitingScan = AwaitingScan{};
      awaitingScan.seq = op.seq;
      awaitingScan.kind = op.kind;
      awaitingScan.target = (uint8_t)op.arg;
      awaitingScan.countBefore = countBefore;
      stagedOp.pending = false;
      return;
    }
    case FollowerOpKind::HomeAll:
      busHomeAll();
      grade = maintGradeWire(0);
      break;
    case FollowerOpKind::BootInfo: {
      BootInfoSlot slot;
      slot.seq = op.seq;
      slot.addr = op.addr;
      slot.ok = busReadBootInfo(op.addr, slot.report);
      slot.done = true;
      bootInfoSlot = slot;
      grade = maintGradeObserved(slot.ok, MaintReason::BootInfoReadFail);
      break;
    }
    default:
      break;
  }
  stampOpResult(op.seq, grade);
  stagedOp.pending = false;
}

// One poll per SELF_TEST_POLL_MS; SelfTestPoll.h decides what the replies
// mean (stale terminal, unsupported firmware, timeout).
static void pollSelfTest() {
  if (!selfTestPolling) return;
  if (millis() - selfTestPollLastMs < SELF_TEST_POLL_MS) return;
  selfTestPollLastMs = millis();
  UnitSelfTestReading reading;
  bool readOk = busReadSelfTest(selfTestSlot.addr, reading);
  SelfTestOutcome outcome = selfTestPollObserve(
      selfTestPoll, readOk, reading, selfTestPollLastMs, selfTestSlot);
  if (outcome == SelfTestOutcome::Pending) return;
  selfTestSlot.outcome = outcome;
  selfTestPolling = false;
  stampOpResult(selfTestSlot.seq,
                maintGradeObserved(outcome == SelfTestOutcome::Ok));
}

bool unitUpdateQueuedOrRunning() {
  return (stagedOp.pending && (stagedOp.kind == FollowerOpKind::ReflashUnit ||
                               stagedOp.kind == FollowerOpKind::BootUpdate)) ||
         reflashInProgress(reflashProgress);
}

void unitJobsLoopTick() {
  if (stagedOp.pending) executeStagedOp();
  if (bootDumpBytes != nullptr && !stagedOp.pending &&
      (uint32_t)(millis() - bootDumpBytesAtMs) >= BOOT_DUMP_KEEP_MS) {
    releaseBootDumpBytes();
  }
  pollSelfTest();
  if (unitHealthRefreshPending) {
    // Probe-inhibit (v1 #88): wait out any twiboot window before scanning.
    if ((int32_t)(millis() - busProbeInhibitedUntilMs()) >= 0) {
      unitHealthRefreshPending = false;
      busProbe();
      busPollHealth();
      if (awaitingScan.seq != 0) {
        MaintGrade grade = maintGradeWire(0);
        if (awaitingScan.kind == FollowerOpKind::SetAddress) {
          grade.outcome = classifySetAddressOutcome(unitFacts, UNITS_AMOUNT, awaitingScan.target,
                                                    grade.reason);
        } else if (awaitingScan.kind == FollowerOpKind::ClearAddress) {
          grade.outcome = classifyClearAddressOutcome(awaitingScan.countBefore,
                                                      detectedUnitCount, grade.reason);
        }
        stampOpResult(awaitingScan.seq, grade);
        awaitingScan = AwaitingScan{};
      }
    }
  }
}
