#pragma once
// FollowerOps.h — the follower's maintenance-op layer (#298), natively
// tested by test_follower_ops. Trimmed COPY of the v2 master's
// MaintenancePolicy.h validators plus the op/self-test/reflash result
// fragments of its DisplayIpc.h and ReflashPlan.h (copy policy: fix shared
// bugs in both trees). Trimmed: no set/clear-address ops (provisioning
// stays a bench job on the S3 master), no display command queue — the
// superloop executes ONE staged op at a time and stamps the single result
// slot the {"seq":N} → GET /unit/op-result contract reads.

#ifdef UNIT_TEST
  #include <cstdint>
  #include <cstdlib>
  #include <cstdio>
#else
  #include <Arduino.h>
#endif

#include "SplitFlapProtocol.h"
#include "UnitHealth.h"
#include "UnitSelfTest.h"  // SELFTEST_REASON_* + selfTestReasonName (#404)

static_assert(SFP_I2C_ADDRESS_BASE == 1,
              "maintenance validators assume address base 1");

// --- validators (v2 MaintenancePolicy.h copies) -----------------------------------

struct MaintVerdict {
  int httpStatus = 200;
  const char* message = "";
};

inline MaintVerdict maintValidateAddress(const char* raw,
                                         const UnitFacts* units, int maxUnits,
                                         int& outAddr) {
  if (raw == nullptr) {
    return {400, "Missing 'address' query param"};
  }
  char* end = nullptr;
  long parsed = strtol(raw, &end, 0);
  if (end == raw) {
    return {400, "Address must be a number"};
  }
  if (parsed < 1 || parsed > 126) {
    return {400, "Address must be 1..126"};
  }
  int unitIndex = (int)parsed - SFP_I2C_ADDRESS_BASE;
  if (unitIndex < 0 || unitIndex >= maxUnits || units[unitIndex].state != 1) {
    return {404, "No sketch-running unit at that address"};
  }
  // #405: a unit reporting a wire contract we do not speak is present but not
  // drivable, so every single-unit op behind this gate is refused. Without it
  // an operator could still fire the most consequential one of all —
  // SET_I2C_ADDRESS, an unverifiable address burn whose recovery is a physical
  // trip — at a unit whose reply layout we cannot even parse.
  //
  // POST /unit/reboot deliberately does NOT come through here (it range-checks
  // itself), which is what keeps ENTER_BOOTLOADER reachable and a mismatched
  // unit always recoverable by reflash.
  if (!unitDrivable(units[unitIndex])) {
    return {409, "Unit reports an unrecognised protocol version — reflash it"};
  }
  outAddr = (int)parsed;
  return {};
}

inline MaintVerdict maintValidateOffset(long value) {
  if (value < -SFP_OFFSET_LIMIT_STEPS || value > SFP_OFFSET_LIMIT_STEPS) {
    return {400, "Offset must be within +/-2038 steps (one revolution)"};
  }
  return {};
}

inline MaintVerdict maintValidateJog(long steps) {
  if (steps < -127 || steps > 127) {
    return {400, "Steps must be -127..127"};
  }
  return {};
}

inline void maintEncodeOffsetLE(int16_t value, uint8_t out[2]) {
  out[0] = (uint8_t)((uint16_t)value & 0xFF);
  out[1] = (uint8_t)(((uint16_t)value >> 8) & 0xFF);
}

// Feature gates are one wire byte (#409). This row's five units are gated the
// same way row 1's sixteen are, or they would need a reflash to enable what
// the S3 rows enable with a curl — so this must reject exactly what the S3's
// maintValidateGates rejects, including the #458 unimplemented-bit check. A
// permissive follower would be the hole the S3 just closed.
inline MaintVerdict maintValidateGates(long gates) {
  if (gates < 0 || gates > 255) {
    return {400, "Gates must be a 0..255 bit mask"};
  }
  if ((gates & ~(long)SFP_UNIT_GATE_IMPLEMENTED) != 0) {
    return {400, "Gates contain bits no unit firmware implements"};
  }
  return {};
}

inline uint8_t maintEncodeJogByte(int steps) {
  if (steps > 127) steps = 127;
  if (steps < -127) steps = -127;
  return (uint8_t)(int8_t)steps;
}

// --- staged op (superloop single slot) ---------------------------------------------

enum class FollowerOpKind : uint8_t {
  None = 0,
  WriteOffset,
  Jog,
  Home,
  Identify,
  ResetOdometer,
  SelfTest,
  RebootToBootloader,
  SetGates,
  ReflashUnit,  // #513: reflash exactly one unit; addr = the target
  BootUpdate,   // #499: in-system twiboot update (reads info, drives stages)
};

// --- execution outcomes (the /unit/op-result vocabulary, v2 copies) ----------------

enum class MaintOutcome : uint8_t {
  Pending = 0,
  Ok,
  WireFail,
  ExecValidationFail,
  PostconditionFail,
};

enum class MaintReason : uint8_t {
  None = 0,
  UnitMissingAfterReprobe,
  TargetAddressOccupied,
  BootInfoReadFail,
  BootStateUnknown,
  BootLockRefused,
  BootUnitLost,
  BootVerifyFailed,
  BootAlreadyNew,
};

struct MaintResult {
  uint32_t seq = 0;
  MaintOutcome outcome = MaintOutcome::Pending;
  MaintReason reason = MaintReason::None;
};

inline const char* maintOutcomeName(MaintOutcome o) {
  switch (o) {
    case MaintOutcome::Ok:                 return "ok";
    case MaintOutcome::WireFail:           return "wire-fail";
    case MaintOutcome::ExecValidationFail: return "exec-validation-fail";
    case MaintOutcome::PostconditionFail:  return "postcondition-fail";
    default:                               return "pending";
  }
}

inline const char* maintReasonName(MaintReason r) {
  switch (r) {
    case MaintReason::UnitMissingAfterReprobe:
      return "unit-missing-after-reprobe";
    case MaintReason::TargetAddressOccupied:
      return "target-address-occupied";
    case MaintReason::BootInfoReadFail:
      return "boot-info-read-fail";
    case MaintReason::BootStateUnknown:
      return "boot-state-unknown";
    case MaintReason::BootLockRefused:
      return "boot-lock-refused";
    case MaintReason::BootUnitLost:
      return "boot-unit-lost";
    case MaintReason::BootVerifyFailed:
      return "boot-verify-failed";
    case MaintReason::BootAlreadyNew:
      return "boot-already-new";
    default:
      return "";
  }
}

enum class OpResultState : uint8_t { Pending = 0, Found, Expired };

inline OpResultState opResultQuery(const MaintResult& slot, uint32_t seq) {
  if (slot.seq < seq ||
      (slot.seq == seq && slot.outcome == MaintOutcome::Pending)) {
    return OpResultState::Pending;
  }
  if (slot.seq > seq) return OpResultState::Expired;
  return OpResultState::Found;
}

inline void buildOpResultJson(char* buf, size_t cap, const MaintResult& slot,
                              uint32_t seq) {
  switch (opResultQuery(slot, seq)) {
    case OpResultState::Pending:
      snprintf(buf, cap, "{\"state\":\"pending\"}");
      return;
    case OpResultState::Expired:
      snprintf(buf, cap, "{\"state\":\"expired\"}");
      return;
    case OpResultState::Found:
      break;
  }
  if (slot.outcome == MaintOutcome::Ok) {
    snprintf(buf, cap, "{\"state\":\"ok\"}");
  } else if (slot.reason == MaintReason::None) {
    snprintf(buf, cap, "{\"state\":\"failed\",\"reason\":\"%s\"}",
             maintOutcomeName(slot.outcome));
  } else {
    snprintf(buf, cap,
             "{\"state\":\"failed\",\"reason\":\"%s\",\"detail\":\"%s\"}",
             maintOutcomeName(slot.outcome), maintReasonName(slot.reason));
  }
}

// --- self-test result slot (#265 contract, v2 copies) ------------------------------

enum class SelfTestOutcome : uint8_t {
  Pending = 0,
  Ok,
  WireFail,
  Timeout,
  UnitFailed,
  Unsupported,
  Aborted,
};

struct SelfTestSlot {
  uint32_t seq = 0;
  SelfTestOutcome outcome = SelfTestOutcome::Pending;
  uint16_t stepsPerRev = 0;
  uint16_t hallWindowSteps = 0;
  uint16_t revTimeMs = 0;
  // Which of the unit's three failure modes fired (#404), SELFTEST_REASON_*.
  // `outcome` is this row master's view; this is the unit's own account of
  // why. Both rows must report identically.
  uint8_t unitReason = SELFTEST_REASON_NONE;
};

inline const char* selfTestOutcomeName(SelfTestOutcome o) {
  switch (o) {
    case SelfTestOutcome::Ok:          return "ok";
    case SelfTestOutcome::WireFail:    return "wire-fail";
    case SelfTestOutcome::Timeout:     return "timeout";
    case SelfTestOutcome::UnitFailed:  return "unit-failed";
    case SelfTestOutcome::Unsupported: return "unsupported";
    case SelfTestOutcome::Aborted:     return "aborted";
    default:                           return "pending";
  }
}

inline void buildSelfTestJson(char* buf, size_t cap, const SelfTestSlot& slot,
                              uint32_t seq) {
  if (slot.seq < seq ||
      (slot.seq == seq && slot.outcome == SelfTestOutcome::Pending)) {
    snprintf(buf, cap, "{\"state\":\"pending\"}");
    return;
  }
  if (slot.seq > seq) {
    snprintf(buf, cap, "{\"state\":\"expired\"}");
    return;
  }
  if (slot.outcome == SelfTestOutcome::Ok) {
    snprintf(buf, cap,
             "{\"state\":\"ok\",\"steps_per_rev\":%u,\"hall_window\":%u,"
             "\"rev_time_ms\":%u}",
             (unsigned)slot.stepsPerRev, (unsigned)slot.hallWindowSteps,
             (unsigned)slot.revTimeMs);
    return;
  }
  // #404: carry the unit's own failure mode and whatever it measured before
  // giving up, exactly as the master's buildSelfTestJson does.
  snprintf(buf, cap,
           "{\"state\":\"failed\",\"reason\":\"%s\",\"unit_reason\":\"%s\","
           "\"steps_per_rev\":%u,\"hall_window\":%u,\"rev_time_ms\":%u}",
           selfTestOutcomeName(slot.outcome),
           selfTestReasonName(slot.unitReason),
           (unsigned)slot.stepsPerRev, (unsigned)slot.hallWindowSteps,
           (unsigned)slot.revTimeMs);
}

// --- reflash progress (#205 shape, v2 ReflashPlan.h copies) -------------------------

// v1 #138 brownout throttle values — the ESP-01 rows are exactly the v1
// hardware the 2-unit batch was tuned on (the S3's batch-4 bump was gated
// on ITS bench).
#define REFLASH_BATCH_SIZE 2
#define REFLASH_BATCH_SETTLE_MS 15000UL
#define TWIBOOT_STARTUP_MS 500

enum class ReflashState : uint8_t {
  Idle = 0,
  Entering,
  Flashing,
  Settling,
  Done,
  Cancelled,
  Failed,
};

struct ReflashProgress {
  ReflashState state = ReflashState::Idle;
  uint8_t total = 0;
  uint8_t done = 0;
  uint8_t failed = 0;
  uint8_t currentAddr = 0;
};

inline bool reflashInProgress(const ReflashProgress& p) {
  return p.state == ReflashState::Entering ||
         p.state == ReflashState::Flashing ||
         p.state == ReflashState::Settling;
}

inline void reflashProgressBegin(ReflashProgress& p, int total) {
  p.state = ReflashState::Entering;
  p.total = (uint8_t)total;
  p.done = 0;
  p.failed = 0;
  p.currentAddr = 0;
}

inline void reflashProgressUnitStart(ReflashProgress& p, uint8_t addr) {
  p.state = ReflashState::Flashing;
  p.currentAddr = addr;
}

inline void reflashProgressUnitResult(ReflashProgress& p, bool ok) {
  if (ok) p.done++; else p.failed++;
}

inline void reflashProgressSettling(ReflashProgress& p) {
  p.state = ReflashState::Settling;
  p.currentAddr = 0;
}

inline void reflashProgressFinish(ReflashProgress& p, bool cancelled) {
  p.currentAddr = 0;
  if (cancelled) {
    p.state = ReflashState::Cancelled;
  } else if (p.failed > 0) {
    p.state = ReflashState::Failed;
  } else {
    p.state = ReflashState::Done;
  }
}

inline const char* reflashStateName(ReflashState s) {
  switch (s) {
    case ReflashState::Entering:  return "entering";
    case ReflashState::Flashing:  return "flashing";
    case ReflashState::Settling:  return "settling";
    case ReflashState::Done:      return "done";
    case ReflashState::Cancelled: return "cancelled";
    case ReflashState::Failed:    return "failed";
    default:                      return "idle";
  }
}

inline void buildReflashJson(char* buf, size_t cap,
                             const ReflashProgress& p) {
  snprintf(buf, cap,
           "{\"state\":\"%s\",\"total\":%u,\"done\":%u,\"failed\":%u,"
           "\"cur\":%u}",
           reflashStateName(p.state), (unsigned)p.total, (unsigned)p.done,
           (unsigned)p.failed, (unsigned)p.currentAddr);
}

// A sketch-mode unit whose rev is not the bundled one gets pushed into
// twiboot; bootloader units are flash targets already (v2 ReflashPlan.h).
inline bool reflashUnitNeedsReboot(const UnitFacts& u) {
  return u.state == 1 && u.fwStatus != 0;
}

inline int reflashCollectRebootTargets(const UnitFacts* facts, int maxUnits,
                                       int base, uint8_t* outAddrs) {
  int n = 0;
  for (int i = 0; i < maxUnits; i++) {
    if (reflashUnitNeedsReboot(facts[i])) outAddrs[n++] = (uint8_t)(base + i);
  }
  return n;
}

// Narrow a collected target list to one address; 0 = no filter (0 is the
// general-call address, never a unit's). Applied to BOTH the reboot sweep and
// the post-rescan flash list, so a unit stranded in twiboot by an earlier
// attempt is not swept up by a run aimed at a different address (#513; the
// S3's ReflashPlan.h carries the same helper for #412).
inline int reflashFilterToAddress(uint8_t* addrs, int n, uint8_t onlyAddr) {
  if (onlyAddr == 0) return n;
  for (int i = 0; i < n; i++) {
    if (addrs[i] == onlyAddr) {
      addrs[0] = onlyAddr;
      return 1;
    }
  }
  return 0;
}

// The one decision that keeps a targeted run off its neighbours: the flash
// loop walks the facts, not the filtered list, so a unit already sitting in
// twiboot at another address is skipped HERE or not at all.
inline bool reflashShouldFlashUnit(const UnitFacts& u, uint8_t addr,
                                   uint8_t onlyAddr) {
  if (u.state != 2) return false;
  return onlyAddr == 0 || addr == onlyAddr;
}

// /reflash-units?address=N bound: the managed range only. Deliberately not
// maintValidateAddress — a unit in twiboot or on a protocol we do not speak
// must still be reflashable; converging it is the point. A target that is
// current, silent or absent plans nothing: the op still reports ok and the
// progress object's total is 0, which is what a caller must check (the S3's
// contract, and what commission-units.sh reads).
inline bool reflashAddressInRange(long addr, int base, int maxUnits) {
  return addr >= base && addr < (long)base + maxUnits;
}

// Decimal digits only, 1..3 of them. This picks which unit gets erased, so no
// base guessing ("010" is not 8) and no trailing garbage ("3abc" is not 3).
inline bool reflashParseAddress(const char* raw, long& out) {
  if (raw == nullptr || raw[0] == '\0') return false;
  long v = 0;
  int n = 0;
  for (; raw[n] != '\0'; n++) {
    if (raw[n] < '0' || raw[n] > '9' || n >= 3) return false;
    v = v * 10 + (raw[n] - '0');
  }
  if (n > 1 && raw[0] == '0') return false;
  out = v;
  return true;
}

// Boot auto-update predicate: only units PROVABLY outdated are
// force-rebooted at boot (v1 #114 rule).
inline int reflashCollectOutdatedTargets(const UnitFacts* facts, int maxUnits,
                                         int base, uint8_t* outAddrs) {
  int n = 0;
  for (int i = 0; i < maxUnits; i++) {
    if (facts[i].state == 1 && facts[i].fwStatus == 1) {
      outAddrs[n++] = (uint8_t)(base + i);
    }
  }
  return n;
}

inline int reflashCollectFlashTargets(const UnitFacts* facts, int maxUnits,
                                      int base, uint8_t* outAddrs) {
  int n = 0;
  for (int i = 0; i < maxUnits; i++) {
    if (facts[i].state == 2) outAddrs[n++] = (uint8_t)(base + i);
  }
  return n;
}
