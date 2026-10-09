#pragma once
// UnitApiJson.h — a unit's facts as the operator API gives them (#559/#572):
// one line of the units table of GET /api/v2/board/{row}, and everything the
// master knows about one unit for GET /api/v2/unit/{row}/{address}. Pure,
// natively tested by test_unit_api_json. The same for the master's own units
// and a row board's, which are the same UnitFacts (UnitFactsJson.h).
//
// The names are readable; what each is in the facts document the boards
// exchange is tests/test_unit_api_keys.py, which fails when the shared
// serializer gains a fact this has no place for.
//
// A fact the unit was not read for is absent (detail: its group may be empty)
// or null (table), never a made-up zero.

#include <ArduinoJson.h>

#include "FlapLetters.h"
#include "JsonCopied.h"
#include "UnitResetCause.h"
#include "UnitVerdict.h"

// The columns of the units table, in order: named once, then a row of values
// a unit.
static const char* const UNIT_TABLE_FIELDS[] = {
    "address", "level", "reason", "a", "b", "state", "rev", "firmware",
    "bootloader", "supplyMv", "supplyMinMv", "shows", "turns", "offset",
};

inline const char* unitApiStateName(uint8_t state) {
  return state == 1 ? "running" : state == 2 ? "bootloader" : "silent";
}

inline const char* unitApiFirmwareName(uint8_t fwStatus) {
  return fwStatus == 0 ? "current" : fwStatus == 1 ? "outdated" : "unknown";
}

inline const char* unitApiHomeName(uint8_t flags) {
  switch (unitBootHomeState(flags)) {
    case 2: return "homed";
    case 1: return "homing";
    default: return "not-homed";
  }
}

// A flap is given as its place on the drum (0 = blank); which character that
// is, is the alphabet's to say (shared/FlapLetters.h, the page's generated
// copy). False for a place the drum does not have.
inline bool unitApiFlap(uint8_t index) { return index < SFP_FLAP_AMOUNT; }

inline bool unitApiShowsKnown(const UnitFacts& u) {
  return u.diagValid && (u.driftFlags & UNIT_DRIFT_FLAG_POSITION_KNOWN) && u.physLetter != 0xFF;
}

inline void unitApiVerdict(JsonObject into, const UnitVerdict& v) {
  into["level"] = verdictLevelName(v.level);
  into["reason"] = unitReasonName(v.reason);
  into["a"] = v.a;
  into["b"] = v.b;
  JsonArray also = into["also"].to<JsonArray>();
  for (UnitReason r : UNIT_REASON_ORDER) {
    if (r != v.reason && (v.all & unitReasonBit(r))) also.add(unitReasonName(r));
  }
}

// One row of the table, in UNIT_TABLE_FIELDS order. `verdict` may be null
// (the board's units are not judged yet).
inline void unitApiTableRow(JsonArray row, const UnitFacts& u, int address,
                            const UnitVerdict* verdict) {
  row.add(address);
  if (verdict != nullptr) {
    row.add(verdictLevelName(verdict->level));
    row.add(unitReasonName(verdict->reason));
    row.add(verdict->a);
    row.add(verdict->b);
  } else {
    for (int i = 0; i < 4; i++) row.add(nullptr);
  }
  row.add(unitApiStateName(u.state));
  if (u.statusValid) {
    row.add(jsonCopied(u.version));
    row.add(unitApiFirmwareName(u.fwStatus));
  } else {
    row.add(nullptr);
    row.add(nullptr);
  }
  if (u.bootVerdict != BOOT_INTEGRITY_UNREAD) row.add(bootIntegrityName(u.bootVerdict));
  else row.add(nullptr);
  if (u.vitalsValid) {
    row.add(u.vitals.vccNow_mV);
    row.add(u.vitals.vccMin_mV);
  } else {
    row.add(nullptr);
    row.add(nullptr);
  }
  if (unitApiShowsKnown(u) && unitApiFlap(u.physLetter)) row.add(u.physLetter);
  else row.add(nullptr);
  if (u.odometerValid) row.add(u.odometer);
  else row.add(nullptr);
  if (u.offsetValid) row.add(u.offset);
  else row.add(nullptr);
}

// Everything about one unit, grouped the way the unit page shows it. `nowMs`
// is on the clock UnitFacts::lastSeenMs counts on.
inline void unitApiDetail(JsonObject out, const UnitFacts& u, int address, uint32_t nowMs,
                          const UnitVerdict* verdict) {
  out["address"] = address;
  out["state"] = unitApiStateName(u.state);
  if (verdict != nullptr) unitApiVerdict(out["verdict"].to<JsonObject>(), *verdict);

  JsonObject fw = out["firmware"].to<JsonObject>();
  JsonObject power = out["power"].to<JsonObject>();
  JsonObject link = out["link"].to<JsonObject>();
  JsonObject drum = out["drum"].to<JsonObject>();
  JsonObject boot = out["bootloader"].to<JsonObject>();

  if (u.statusValid) {
    const UnitStatus& s = u.status;
    fw["rev"] = jsonCopied(u.version);
    fw["status"] = unitApiFirmwareName(u.fwStatus);
    fw["uptimeS"] = unitVerdictUptimeS(u);
    power["brownouts"] = s.lifetimeBrownoutCount;
    power["watchdogResets"] = s.lifetimeWatchdogCount;
    power["lastStart"] = unitResetKindName(unitResetFromStatusByte(s.mcusrAtBoot));
    power["restartedWhileWatched"] = u.resetSeen;
    link["badCommands"] = s.badCommandCount;
    drum["homeSteps"] = s.lastHomingStepCount;
    drum["homeFailed"] = (s.flags & UNIT_FLAG_LAST_HOME_FAILED) != 0;
    drum["hallNeverSeen"] = (s.flags & UNIT_FLAG_HALL_NEVER) != 0;
    drum["moving"] = (s.flags & UNIT_FLAG_MOVING) != 0;
    out["addressStored"] = (s.flags & UNIT_FLAG_ADDR_EEPROM) != 0;
  }
  if (u.protocolKnown) {
    fw["protocol"] = u.protocolVersion;
    fw["protocolSupported"] = unitProtocolSupported(u.protocolVersion);
  }
  if (u.vitalsValid) {
    power["supplyMv"] = u.vitals.vccNow_mV;
    power["supplyMinMv"] = u.vitals.vccMin_mV;
    power["freeRamMin"] = u.vitals.freeRamMin;
    if (unitApiFlap(u.vitals.cmdPos)) drum["commanded"] = u.vitals.cmdPos;
  }
  if (u.extDiagValid) {
    const UnitExtDiag& e = u.extDiag;
    power["supplyDuringLastMoveMv"] = e.vccSagLastMove;
    drum["homeExcessSteps"] = e.stepExcessLast;
    drum["homeExcessStepsMax"] = e.stepExcessMax;
    drum["hallEdgesLastTurn"] = e.hallEdgesLastRev;
    drum["movesInWindow"] = e.dutyWindow;
    drum["jammed"] = (e.statusBits & EXT_DIAG_STATUS_STALL) != 0;
    drum["heldForSupply"] = (e.statusBits & EXT_DIAG_STATUS_SUPPLY_WAIT) != 0;
  }
  if (u.linkValid) {
    link["received"] = u.link.rxFrames;
    link["answered"] = u.link.txReplies;
    link["selfRepairs"] = u.link.deafHeals;
  }
  if (u.state == 1) {
    link["heardMsAgo"] = (uint32_t)(nowMs - u.lastSeenMs);
    link["missed"] = u.misses;
    link["lost"] = u.stale;
    link["failed"] = u.i2cErrors;
    if (u.i2cErrors > 0) link["failedMsAgo"] = (uint32_t)(nowMs - u.lastErrorMs);
    link["rescuedFromBootloader"] = u.rescueExits;
    drum["home"] = unitApiHomeName(u.status.flags);
  }
  if (u.offsetValid) drum["offset"] = u.offset;
  if (u.odometerValid) drum["turns"] = u.odometer;
  if (u.diagValid) {
    drum["slips"] = u.driftEvents;
    drum["lastSlipSteps"] = u.lastDriftSteps;
    drum["rehomePending"] = (u.driftFlags & UNIT_DRIFT_FLAG_PENDING) != 0;
    if (unitApiShowsKnown(u) && unitApiFlap(u.physLetter)) {
      drum["shows"] = u.physLetter;
      drum["wrongLetter"] = u.mismatch;
    }
  }
  if (u.lifetimeValid) {
    const UnitLifetimeFacts& lt = u.lifetime;
    drum["homeFailures"] = lt.homeFailedCount;
    drum["homeExcessStepsEver"] = lt.stepExcessLifetimeMax;
    drum["futileRehomes"] = lt.idleHallFutileRehomes;
    drum["hallCheckOff"] = lt.idleHallStoodDown;
    fw["gates"] = lt.featureGates;
    if (lt.selfTestFirstHallWindow || lt.selfTestLastHallWindow || lt.selfTestFirstStepsPerRev ||
        lt.selfTestLastStepsPerRev) {
      JsonObject test = drum["selfTest"].to<JsonObject>();
      test["firstHallWindow"] = lt.selfTestFirstHallWindow;
      test["lastHallWindow"] = lt.selfTestLastHallWindow;
      test["firstStepsPerTurn"] = lt.selfTestFirstStepsPerRev;
      test["lastStepsPerTurn"] = lt.selfTestLastStepsPerRev;
    }
  }
  if (u.bootVerdict != BOOT_INTEGRITY_UNREAD) {
    boot["verdict"] = bootIntegrityName(u.bootVerdict);
    char crc[9];
    snprintf(crc, sizeof(crc), "%08lx", (unsigned long)u.bootCrc32);
    if (u.bootVerdict != BOOT_INTEGRITY_OK) boot["crc32"] = jsonCopied(crc);
  }
  // What the bootloader said about itself, known only for a unit found in it.
  const TwibootIdentity& b = u.bootloader;
  if (b.generation != TWIBOOT_GEN_UNREAD) {
    boot["generation"] = b.generation;
    boot["capabilities"] = b.caps;
    if (b.fusesValid) {
      char hex[7];
      snprintf(hex, sizeof(hex), "%02x", b.lock);
      boot["lock"] = jsonCopied(hex);
      snprintf(hex, sizeof(hex), "%02x%02x%02x", b.lfuse, b.hfuse, b.efuse);
      boot["fuses"] = jsonCopied(hex);
    }
    if (b.crashValid) boot["crashes"] = b.crashCount;
  }
}
