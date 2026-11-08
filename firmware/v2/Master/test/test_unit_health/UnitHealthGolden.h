#pragma once
// Fingerprints of buildUnitHealthJson's output over pseudo-random rows (every
// valid flag, state and emit-when-nonzero value toggled) and over every cap
// of a short row, truncation included. The document is an external contract
// (docs/HA_CONTRACT.md): the expected values in test_main.cpp were taken from
// the serializer before it was split into per-block appenders, so a change
// to any byte, or to the length it reports for a short buffer, fails.

#include <cstdint>
#include <cstring>

#include "UnitHealth.h"

struct GoldenRng {
  uint32_t s;
  uint32_t next() {
    s = s * 1664525UL + 1013904223UL;
    return s >> 8;
  }
  bool coin() { return (next() & 1) != 0; }
  // Mostly small, sometimes zero, sometimes the type's widest.
  uint32_t value(uint32_t max) {
    switch (next() % 4) {
      case 0: return 0;
      case 1: return max;
      default: return max == 0xFFFFFFFFUL ? (next() << 8) ^ next() : next() % (max + 1);
    }
  }
};

inline uint32_t goldenFnv(uint32_t h, const void* data, size_t len) {
  const uint8_t* p = (const uint8_t*)data;
  for (size_t i = 0; i < len; i++) {
    h ^= p[i];
    h *= 16777619UL;
  }
  return h;
}

inline void goldenRandomStatus(GoldenRng& r, UnitFacts& u) {
  u.statusValid = r.coin();
  u.fwStatus = (uint8_t)r.value(2);
  strcpy(u.rev, r.coin() ? "abc12345" : "");
  u.status.flags = (uint8_t)r.value(0xFF);
  u.status.mcusrAtBoot = (uint8_t)r.value(0xFF);
  u.status.lifetimeBrownoutCount = (uint8_t)r.value(0xFF);
  u.status.lifetimeWatchdogCount = (uint8_t)r.value(0xFF);
  u.status.uptimeSeconds = (uint16_t)r.value(0xFFFF);
  u.status.badCommandCount = (uint8_t)r.value(0xFF);
  u.status.lastHomingStepCount = (uint16_t)r.value(0xFFFF);
  u.resetSeen = r.coin();
  u.odometerValid = r.coin();
  u.odometer = r.value(0xFFFFFFFFUL);
  u.offsetValid = r.coin();
  u.offset = (int16_t)(r.value(0xFFFF) - 0x8000);
  u.diagValid = r.coin();
  u.physLetter = r.coin() ? 0xFF : (uint8_t)r.value(44);
  u.driftFlags = (uint8_t)r.value(3);
  u.driftEvents = (uint8_t)r.value(0xFF);
  u.lastDriftSteps = (int8_t)(r.value(0xFF) - 0x80);
  u.mismatch = r.coin();
}

inline void goldenRandomDiag(GoldenRng& r, UnitFacts& u) {
  u.vitalsValid = r.coin();
  u.vitals.vccNow_mV = (uint16_t)r.value(0xFFFF);
  u.vitals.vccMin_mV = (uint16_t)r.value(0xFFFF);
  u.vitals.cmdPos = (uint8_t)r.value(0xFF);
  u.vitals.freeRamMin = (uint16_t)r.value(0xFFFF);
  u.extDiagValid = r.coin();
  u.extDiag.stepExcessLast = (uint16_t)r.value(0xFFFF);
  u.extDiag.stepExcessMax = (uint16_t)r.value(0xFFFF);
  u.extDiag.vccSagLastMove = (uint16_t)r.value(0xFFFF);
  u.extDiag.hallEdgesLastRev = (uint8_t)r.value(0xFF);
  u.extDiag.dutyWindow = (uint16_t)r.value(0xFFFF);
  u.extDiag.statusBits = (uint8_t)r.value(0xFF);
  u.linkValid = r.coin();
  u.link.uptimeSeconds = r.value(0xFFFFFFFFUL);
  u.link.rxFrames = (uint16_t)r.value(0xFFFF);
  u.link.txReplies = (uint16_t)r.value(0xFFFF);
  u.link.deafHeals = (uint8_t)r.value(0xFF);
  u.protocolKnown = r.coin();
  u.protocolVersion = (uint8_t)r.value(SFP_PROTOCOL_VERSION + 1);
}

inline void goldenRandomRecords(GoldenRng& r, UnitFacts& u) {
  u.lifetimeValid = r.coin();
  u.lifetime.homeFailedCount = (uint8_t)r.value(0xFF);
  u.lifetime.featureGates = (uint8_t)r.value(0xFF);
  u.lifetime.stepExcessLifetimeMax = (uint16_t)r.value(0xFFFF);
  u.lifetime.selfTestFirstHallWindow = (uint16_t)r.value(0xFFFF);
  u.lifetime.selfTestFirstStepsPerRev = (uint16_t)r.value(0xFFFF);
  u.lifetime.selfTestLastHallWindow = (uint16_t)r.value(0xFFFF);
  u.lifetime.selfTestLastStepsPerRev = (uint16_t)r.value(0xFFFF);
  u.lifetime.idleHallFutileRehomes = (uint8_t)r.value(0xFF);
  u.lifetime.idleHallStoodDown = r.coin();
  u.busRecordValid = r.coin();
  u.busRecord.silences = (uint8_t)r.value(0xFF);
  u.busRecord.lastMinutes = (uint8_t)r.value(0xFF);
  u.busRecord.longestMinutes = (uint8_t)r.value(0xFF);
  u.busRecord.reinits = (uint8_t)r.value(0xFF);
  u.busRecord.reinitsHeard = (uint8_t)r.value(0xFF);
  u.busRecord.selfRestarts = (uint8_t)r.value(0xFF);
  u.busRecord.flags = (uint8_t)r.value(0xFF);
  u.busSilentNowMinutes = (uint8_t)r.value(0xFF);
}

inline void goldenRandomLiveness(GoldenRng& r, UnitFacts& u) {
  u.state = (uint8_t)(r.next() % 3);
  u.lastSeenMs = r.value(0xFFFFFFFFUL);
  u.misses = (uint8_t)r.value(0xFF);
  u.stale = r.coin();
  u.i2cErrors = (uint16_t)r.value(0xFFFF);
  u.lastErrorMs = r.value(0xFFFFFFFFUL);
  u.rescueExits = (uint16_t)r.value(0xFFFF);
  u.bootVerdict = (uint8_t)(r.next() % 4);
  u.bootCrc32 = r.value(0xFFFFFFFFUL);
  u.bootloader.generation = (uint8_t)r.value(0xFF);
  u.bootloader.caps = (uint8_t)r.value(0xFF);
  u.bootloader.fusesValid = r.coin();
  u.bootloader.lock = (uint8_t)r.value(0xFF);
  u.bootloader.lfuse = (uint8_t)r.value(0xFF);
  u.bootloader.hfuse = (uint8_t)r.value(0xFF);
  u.bootloader.efuse = (uint8_t)r.value(0xFF);
  u.bootloader.crashValid = r.coin();
  u.bootloader.crashCount = (uint8_t)r.value(4);
}

inline UnitFacts goldenRandomUnit(GoldenRng& r) {
  UnitFacts u;
  goldenRandomStatus(r, u);
  goldenRandomDiag(r, u);
  goldenRandomRecords(r, u);
  goldenRandomLiveness(r, u);
  return u;
}

// One hash over `rows` full documents of 16 pseudo-random units each, the
// returned length folded in.
inline uint32_t goldenFullDocsHash(uint32_t seed, int rows) {
  static char buf[UNIT_HEALTH_JSON_CAP];
  GoldenRng r{seed};
  uint32_t h = 2166136261UL;
  for (int row = 0; row < rows; row++) {
    UnitFacts units[16];
    for (int i = 0; i < 16; i++) units[i] = goldenRandomUnit(r);
    const int width = 1 + (int)(r.next() % 16);
    const uint32_t nowMs = r.next();
    const size_t n = buildUnitHealthJson(buf, sizeof buf, units, width,
                                         (int)(r.next() % 17), 1, nowMs);
    h = goldenFnv(h, &n, sizeof n);
    h = goldenFnv(h, buf, n);
  }
  return h;
}

// Every cap from 0 to past the end of a short row: the returned length and
// every byte the serializer left in the buffer.
inline uint32_t goldenCapSweepHash(uint32_t seed) {
  static char buf[4096];
  GoldenRng r{seed};
  UnitFacts units[3];
  for (int i = 0; i < 3; i++) units[i] = goldenRandomUnit(r);
  units[0].statusValid = true;
  units[0].state = 1;
  const size_t full = buildUnitHealthJson(buf, sizeof buf, units, 3, 1, 1, 5000);
  uint32_t h = 2166136261UL;
  for (size_t cap = 0; cap <= full + 2; cap++) {
    memset(buf, 0xA5, sizeof buf);
    const size_t n = buildUnitHealthJson(buf, cap, units, 3, 1, 1, 5000);
    h = goldenFnv(h, &n, sizeof n);
    h = goldenFnv(h, buf, cap + 1);
  }
  return h;
}
