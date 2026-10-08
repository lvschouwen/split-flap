#pragma once
// UnitFactsWidest.h — the widest unit the facts serializer can write (#567).
//
// Every buffer that holds buildUnitHealthJson's document is sized by
// unitFactsDocCap() in UnitHealth.h, and that figure is held to the units
// built here by the native tests of both boards. A unit answers either from
// its firmware (state 1: the freshness keys) or from its bootloader (state 2:
// the bl* keys), never both, so there are two.
//
// UNIT_FACTS_KEYS names every key the serializer writes. The native test
// finds each one in the two documents; tests/test_unit_facts_widest.py holds
// the list to the serializer's source, so a key added there without a value
// here fails before it can outgrow a buffer unnoticed.

#include <stdio.h>
#include <string.h>

#include "ReflashPlan.h"
#include "UnitHealth.h"
#include "WearPolicy.h"

// With lastSeenMs and lastErrorMs at 0 this makes both ages ten digits.
#define UNIT_FACTS_WIDEST_NOW_MS 0xFFFFFFFFUL

static const char* const UNIT_FACTS_KEYS[] = {
    "width", "faulty", "vccMin", "units", "i",    "a",     "st",   "v",    "fw",   "rev",
    "up",    "br",     "wd",     "bc",    "mc",   "fl",    "hs",   "ae",   "rs",   "odo",
    "ofs",   "de",     "ds",     "dp",    "phys", "mm",    "vcc",  "vmin", "cp",   "ram",
    "se",    "sx",     "sag",    "he",    "dw",   "sb",    "ut",   "rx",   "tx",   "dh",
    "pv",    "pmm",    "hf",     "gates", "sxl",  "stw0",  "stw1", "str0", "str1", "fr",
    "frd",   "age",    "hs2",    "misses", "stale", "err", "errAge", "rsx", "bv",  "bcrc",
    "blv",   "blc",    "blk",    "blf",   "blx",
};
#define UNIT_FACTS_KEY_COUNT (sizeof(UNIT_FACTS_KEYS) / sizeof(UNIT_FACTS_KEYS[0]))

// Everything a unit can report whatever answers at its address.
inline UnitFacts unitFactsWidestCommon() {
  UnitFacts u;
  u.statusValid = true;
  u.fwStatus = 2;
  strcpy(u.version, "abc12345");
  u.status.flags = 0xFF;
  u.status.mcusrAtBoot = 255;
  u.status.lifetimeBrownoutCount = 255;
  u.status.lifetimeWatchdogCount = 255;
  u.status.uptimeSeconds = 65535;
  u.status.badCommandCount = 255;
  u.status.lastHomingStepCount = 65535;
  u.resetSeen = true;
  u.odometer = 0xFFFFFFFFUL;
  u.odometerValid = true;
  u.offset = -32768;
  u.offsetValid = true;
  u.diagValid = true;
  u.physLetter = 44;
  u.driftFlags = UNIT_DRIFT_FLAG_PENDING | UNIT_DRIFT_FLAG_POSITION_KNOWN;
  u.driftEvents = 255;
  u.lastDriftSteps = -127;
  u.mismatch = true;
  u.vitalsValid = true;
  u.vitals.vccNow_mV = 65535;
  u.vitals.vccMin_mV = 65534;  // 65535 reads as "none" in the fleet minimum
  u.vitals.cmdPos = 255;
  u.vitals.freeRamMin = 65535;
  u.extDiagValid = true;
  u.extDiag.stepExcessLast = 0xFFFF;
  u.extDiag.stepExcessMax = 0xFFFF;
  u.extDiag.vccSagLastMove = 0xFFFF;
  u.extDiag.hallEdgesLastRev = 0xFF;
  u.extDiag.dutyWindow = 0xFFFF;
  u.extDiag.statusBits = 0xFF;
  u.linkValid = true;
  u.link.uptimeSeconds = 0xFFFFFFFFUL;
  u.link.rxFrames = 0xFFFF;
  u.link.txReplies = 0xFFFF;
  u.link.deafHeals = 0xFF;
  u.protocolKnown = true;
  u.protocolVersion = 255;  // one no build speaks: the pmm key too
  u.lifetimeValid = true;
  u.lifetime.homeFailedCount = 0xFF;
  u.lifetime.featureGates = 0xFF;
  u.lifetime.stepExcessLifetimeMax = 0xFFFF;
  u.lifetime.selfTestFirstHallWindow = 0xFFFF;
  u.lifetime.selfTestFirstStepsPerRev = 0xFFFF;
  u.lifetime.selfTestLastHallWindow = 0xFFFF;
  u.lifetime.selfTestLastStepsPerRev = 0xFFFF;
  u.lifetime.idleHallFutileRehomes = 0xFF;
  u.lifetime.idleHallStoodDown = true;
  u.bootVerdict = BOOT_INTEGRITY_CORRUPT;
  u.bootCrc32 = 0xFFFFFFFFUL;
  return u;
}

// A unit answering from its firmware, lost and with errors charged to it.
inline UnitFacts unitFactsWidest() {
  UnitFacts u = unitFactsWidestCommon();
  u.state = 1;
  u.lastSeenMs = 0;
  u.misses = 255;
  u.stale = true;
  u.i2cErrors = 0xFFFF;
  u.lastErrorMs = 0;
  u.rescueExits = 0xFFFF;
  return u;
}

// A unit held in its bootloader, with what was read of it before.
inline UnitFacts unitFactsWidestInBootloader() {
  UnitFacts u = unitFactsWidestCommon();
  u.state = 2;
  u.bootloader.generation = 254;
  u.bootloader.caps = 0xFF;
  u.bootloader.fusesValid = true;
  u.bootloader.crashValid = true;
  u.bootloader.crashCount = 255;
  return u;
}

// A row of `width` such units as a board sends it: the units, then the wear
// and the unit-update objects spliced in before the closing brace, each at
// its widest. Addresses have two digits throughout (a row's go up to 16).
// Returns the length, or 0 when `cap` cannot hold it.
inline size_t unitFactsWidestDoc(char* buf, size_t cap, int width, bool inBootloader) {
  UnitFacts units[16];
  for (int i = 0; i < 16; i++) {
    units[i] = inBootloader ? unitFactsWidestInBootloader() : unitFactsWidest();
  }
  size_t n = buildUnitHealthJson(buf, cap, units, width, 16, 10, UNIT_FACTS_WIDEST_NOW_MS);
  if (n == 0 || n >= cap) return 0;

  WearAssessment wear;
  wear.median = 0xFFFFFFFFUL;
  for (int i = 0; i < 16; i++) wear.flagged[i] = true;
  wear.flaggedCount = 16;
  char wearJson[96];
  const size_t wearLen = buildWearJson(wear, wearJson, sizeof wearJson);
  if (wearLen == 0 || n + wearLen + 2 >= cap) return 0;
  n += (size_t)snprintf(buf + n - 1, cap - n + 1, ",%s}", wearJson) - 1;

  ReflashProgress update;
  update.state = ReflashState::BootUpdate;
  update.total = update.done = update.failed = 255;
  update.currentAddr = 255;
  update.bootDone = update.bootFailed = 255;
  char updateJson[REFLASH_JSON_CAP];
  buildReflashJson(updateJson, sizeof updateJson, update);
  if (n + strlen(updateJson) + 13 >= cap) return 0;
  n += (size_t)snprintf(buf + n - 1, cap - n + 1, ",\"reflash\":%s}", updateJson) - 1;
  return n;
}
