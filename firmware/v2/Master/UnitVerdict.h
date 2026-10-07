#pragma once
// UnitVerdict.h — what the master makes of one unit (#559/#570): a level
// (working / note / fault), the reason that leads, two numbers that belong
// to it, and every reason that applies. Pure, natively tested by
// test_unit_verdict. The same judgement serves the master's own units and a
// row board's, which arrive as the same UnitFacts (UnitFactsJson.h).
//
// A fault is something wrong now: the flap cannot be trusted to show its
// letter, or the unit's way back (its bootloader) is damaged. A note is worth
// knowing and needs nothing done today. History is never a fault: a unit that
// restarted by itself and runs again is a note.
//
// A reason's number is stored in the event record and its name is the API's.
// Neither changes; a new reason is added at the end. The order reasons lead
// in is UNIT_REASON_ORDER, not their numbers.
//
// The wording is the browser's, from the reason and its numbers.

#include <stdint.h>

#include "UnitEventLog.h"  // the supply floor and the drag threshold
#include "UnitHealth.h"

enum class VerdictLevel : uint8_t { Working = 0, Note = 1, Fault = 2 };

inline const char* verdictLevelName(VerdictLevel level) {
  switch (level) {
    case VerdictLevel::Working: return "working";
    case VerdictLevel::Note: return "note";
    case VerdictLevel::Fault: return "fault";
  }
  return "?";
}

//                                   a                        b
enum class UnitReason : uint8_t {
  Working = 0,             // seconds it has run
  NoUnit = 1,              // nothing answers at this place
  NotAnswering = 2,        // seconds since it last did     reads missed
  HeldInBootloader = 3,    // crashes counted
  InBootloader = 4,
  WrongProtocol = 5,       // the contract it reports
  BootloaderDamaged = 6,   // its boot section's CRC-32
  HomeFailed = 7,          // lifetime failures, 0 = unknown
  HallNever = 8,
  BeingUpdated = 9,
  FindingHome = 10,        // 0 waiting, 1 turning
  Jammed = 11,
  WrongLetter = 12,        // the letter it stands at
  LowSupply = 13,          // lowest mV since it started   the floor, mV
  RestartedByItself = 14,  // lifetime brownouts            lifetime watchdog resets
  FirmwareOutdated = 15,
  BootloaderOutdated = 16, // its boot section's CRC-32
  Dragging = 17,           // worst excess steps to home    the threshold
  HallAnomaly = 18,        // hall edges in the last turn
  Worn = 19,               // turns of the drum
  NotRead = 20,
  HomeFailedBefore = 21,   // lifetime failures             seconds it has run
};
#define UNIT_REASON_COUNT 22

inline uint32_t unitReasonBit(UnitReason r) { return 1UL << (uint8_t)r; }

inline const char* unitReasonName(UnitReason r) {
  switch (r) {
    case UnitReason::Working: return "working";
    case UnitReason::NoUnit: return "no-unit";
    case UnitReason::NotAnswering: return "not-answering";
    case UnitReason::HeldInBootloader: return "held-in-bootloader";
    case UnitReason::InBootloader: return "in-bootloader";
    case UnitReason::WrongProtocol: return "wrong-protocol";
    case UnitReason::BootloaderDamaged: return "bootloader-damaged";
    case UnitReason::HomeFailed: return "home-failed";
    case UnitReason::HallNever: return "hall-never";
    case UnitReason::BeingUpdated: return "being-updated";
    case UnitReason::FindingHome: return "finding-home";
    case UnitReason::Jammed: return "jammed";
    case UnitReason::WrongLetter: return "wrong-letter";
    case UnitReason::LowSupply: return "low-supply";
    case UnitReason::RestartedByItself: return "restarted-by-itself";
    case UnitReason::FirmwareOutdated: return "firmware-outdated";
    case UnitReason::BootloaderOutdated: return "bootloader-outdated";
    case UnitReason::Dragging: return "dragging";
    case UnitReason::HallAnomaly: return "hall-anomaly";
    case UnitReason::Worn: return "worn";
    case UnitReason::NotRead: return "not-read";
    case UnitReason::HomeFailedBefore: return "home-failed-before";
  }
  return "?";
}

inline VerdictLevel unitReasonLevel(UnitReason r) {
  switch (r) {
    case UnitReason::Working:
      return VerdictLevel::Working;
    case UnitReason::NoUnit:
    case UnitReason::NotAnswering:
    case UnitReason::HeldInBootloader:
    case UnitReason::InBootloader:
    case UnitReason::WrongProtocol:
    case UnitReason::BootloaderDamaged:
    case UnitReason::HomeFailed:
    case UnitReason::HallNever:
      return VerdictLevel::Fault;
    default:
      return VerdictLevel::Note;
  }
}

// The order reasons lead in: the first that applies names the verdict.
static const UnitReason UNIT_REASON_ORDER[] = {
    UnitReason::NoUnit,
    UnitReason::NotAnswering,
    UnitReason::HeldInBootloader,
    UnitReason::InBootloader,
    UnitReason::WrongProtocol,
    UnitReason::BootloaderDamaged,
    UnitReason::HallNever,
    UnitReason::HomeFailed,
    UnitReason::BeingUpdated,
    UnitReason::FindingHome,
    UnitReason::Jammed,
    UnitReason::WrongLetter,
    UnitReason::LowSupply,
    UnitReason::RestartedByItself,
    UnitReason::FirmwareOutdated,
    UnitReason::BootloaderOutdated,
    UnitReason::Dragging,
    UnitReason::HallAnomaly,
    UnitReason::Worn,
    UnitReason::NotRead,
    UnitReason::HomeFailedBefore,
};

// What the unit's facts do not say about it.
struct UnitVerdictContext {
  uint32_t nowMs = 0;     // on the clock UnitFacts::lastSeenMs counts on
  bool updating = false;  // its board is updating units: a bootloader is expected
  bool worn = false;      // its drum has turned far more than its row's (WearPolicy.h)
};

struct UnitVerdict {
  VerdictLevel level = VerdictLevel::Working;
  UnitReason reason = UnitReason::Working;
  uint32_t a = 0;
  uint32_t b = 0;
  uint32_t all = 0;  // unitReasonBit() of every reason that applies
};

inline uint32_t unitVerdictUptimeS(const UnitFacts& u) {
  if (u.linkValid) return u.link.uptimeSeconds;
  return u.statusValid ? u.status.uptimeSeconds : 0;
}

// Every reason that applies to the unit, as bits.
inline uint32_t unitReasonsOf(const UnitFacts& u, const UnitVerdictContext& ctx) {
  if (u.state == 0) return unitReasonBit(UnitReason::NoUnit);
  if (u.state == 2) {
    if (twibootHeldForCrashing(u.bootloader)) return unitReasonBit(UnitReason::HeldInBootloader);
    return unitReasonBit(ctx.updating ? UnitReason::BeingUpdated : UnitReason::InBootloader);
  }
  // What a unit last said before it went quiet says nothing about it now, and
  // a unit on another contract is never read at all.
  if (unitIsLost(u)) return unitReasonBit(UnitReason::NotAnswering);
  if (!unitDrivable(u)) return unitReasonBit(UnitReason::WrongProtocol);

  uint32_t all = 0;
  const auto add = [&all](UnitReason r) { all |= unitReasonBit(r); };
  if (u.bootVerdict == BOOT_INTEGRITY_CORRUPT) add(UnitReason::BootloaderDamaged);
  if (u.bootVerdict == BOOT_INTEGRITY_OUTDATED) add(UnitReason::BootloaderOutdated);
  bool homeFailedNow = false;
  if (u.statusValid) {
    if (u.status.flags & UNIT_FLAG_HALL_NEVER) add(UnitReason::HallNever);
    if (u.status.flags & UNIT_FLAG_LAST_HOME_FAILED) add(UnitReason::HomeFailed);
    homeFailedNow = unitStatusIsFaulty(u.status);
    if (!homeFailedNow && unitBootHomeState(u.status.flags) != 2) add(UnitReason::FindingHome);
    if (u.resetSeen) add(UnitReason::RestartedByItself);
  } else {
    add(UnitReason::NotRead);
  }
  if (u.extDiagValid) {
    if (u.extDiag.statusBits & EXT_DIAG_STATUS_STALL) add(UnitReason::Jammed);
    if (u.extDiag.stepExcessMax > EXT_DIAG_DRAG_EXCESS_STEPS) add(UnitReason::Dragging);
    if (u.extDiag.hallEdgesLastRev > 1) add(UnitReason::HallAnomaly);
  }
  if (u.diagValid && u.mismatch) add(UnitReason::WrongLetter);
  if (unitVccIsLow(u.vitalsValid, u.vitals.vccMin_mV, UNIT_VCC_MIN_FLOOR_MV)) {
    add(UnitReason::LowSupply);
  }
  if (u.fwStatus == 1) add(UnitReason::FirmwareOutdated);
  if (ctx.worn) add(UnitReason::Worn);
  if (!homeFailedNow && u.lifetimeValid && u.lifetime.homeFailedCount > 0) {
    add(UnitReason::HomeFailedBefore);
  }
  return all;
}

// The reasons this read of the unit can speak for, as bits. One it cannot
// (a status that did not arrive, a unit gone quiet) is neither there nor
// gone: whoever follows a unit over time keeps what it knew.
inline uint32_t unitReasonsObservable(const UnitFacts& u) {
  const auto flag = unitReasonBit;
  // What kind of thing answers at the address is always known.
  const uint32_t standing = flag(UnitReason::NoUnit) | flag(UnitReason::NotAnswering) |
                            flag(UnitReason::HeldInBootloader) | flag(UnitReason::InBootloader) |
                            flag(UnitReason::WrongProtocol) | flag(UnitReason::BeingUpdated);
  const uint32_t every = (1UL << UNIT_REASON_COUNT) - 1;
  // Nothing runs there, so nothing about a running unit holds any more.
  if (u.state != 1) return every;
  if (unitIsLost(u) || !unitDrivable(u)) return standing;
  uint32_t seen = standing | flag(UnitReason::NotRead) | flag(UnitReason::FirmwareOutdated) |
                  flag(UnitReason::Worn);
  if (u.statusValid) {
    seen |= flag(UnitReason::HomeFailed) | flag(UnitReason::HallNever) |
            flag(UnitReason::FindingHome) | flag(UnitReason::RestartedByItself);
  }
  if (u.bootVerdict != BOOT_INTEGRITY_UNREAD) {
    seen |= flag(UnitReason::BootloaderDamaged) | flag(UnitReason::BootloaderOutdated);
  }
  if (u.extDiagValid) {
    seen |= flag(UnitReason::Jammed) | flag(UnitReason::Dragging) | flag(UnitReason::HallAnomaly);
  }
  if (u.diagValid) seen |= flag(UnitReason::WrongLetter);
  if (u.vitalsValid) seen |= flag(UnitReason::LowSupply);
  if (u.statusValid && u.lifetimeValid) seen |= flag(UnitReason::HomeFailedBefore);
  return seen;
}

// The two numbers that belong to a reason (the table at UnitReason).
inline void unitReasonNumbers(UnitReason r, const UnitFacts& u, const UnitVerdictContext& ctx,
                              uint32_t& a, uint32_t& b) {
  a = 0;
  b = 0;
  switch (r) {
    case UnitReason::Working:
      a = unitVerdictUptimeS(u);
      break;
    case UnitReason::NotAnswering:
      a = (uint32_t)(ctx.nowMs - u.lastSeenMs) / 1000UL;
      b = u.misses;
      break;
    case UnitReason::HeldInBootloader:
      a = u.bootloader.crashCount;
      break;
    case UnitReason::WrongProtocol:
      a = u.protocolVersion;
      break;
    case UnitReason::BootloaderDamaged:
    case UnitReason::BootloaderOutdated:
      a = u.bootCrc32;
      break;
    case UnitReason::HomeFailed:
      a = u.lifetimeValid ? u.lifetime.homeFailedCount : 0;
      break;
    case UnitReason::FindingHome:
      a = unitBootHomeState(u.status.flags);
      break;
    case UnitReason::WrongLetter:
      a = u.physLetter;
      break;
    case UnitReason::LowSupply:
      a = u.vitals.vccMin_mV;
      b = UNIT_VCC_MIN_FLOOR_MV;
      break;
    case UnitReason::RestartedByItself:
      a = u.status.lifetimeBrownoutCount;
      b = u.status.lifetimeWatchdogCount;
      break;
    case UnitReason::Dragging:
      a = u.extDiag.stepExcessMax;
      b = EXT_DIAG_DRAG_EXCESS_STEPS;
      break;
    case UnitReason::HallAnomaly:
      a = u.extDiag.hallEdgesLastRev;
      break;
    case UnitReason::Worn:
      a = u.odometer;
      break;
    case UnitReason::HomeFailedBefore:
      a = u.lifetime.homeFailedCount;
      b = unitVerdictUptimeS(u);
      break;
    default:
      break;
  }
}

inline UnitReason unitReasonLeading(uint32_t all) {
  for (UnitReason r : UNIT_REASON_ORDER) {
    if (all & unitReasonBit(r)) return r;
  }
  return UnitReason::Working;
}

inline UnitVerdict unitVerdict(const UnitFacts& u, const UnitVerdictContext& ctx) {
  UnitVerdict v;
  v.all = unitReasonsOf(u, ctx);
  v.reason = unitReasonLeading(v.all);
  v.level = unitReasonLevel(v.reason);
  unitReasonNumbers(v.reason, u, ctx, v.a, v.b);
  return v;
}
