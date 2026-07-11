#pragma once

// Pure unit-health logic — v2 adaptation of v1's UnitHealth.h (copy policy:
// fix shared bugs in both trees). Same UnitStatus decode target, faulty
// predicate and JSON wire shape as v1's /units/health; the difference is the
// input layout: v1 kept struct-of-arrays globals + a prebuilt JSON cache
// (ESP-01 RAM tactic), v2 carries per-unit UnitFacts inside the
// DisplaySnapshot and the web layer renders JSON from its mutex copy via
// buildUnitHealthJson(). No networking and no Wire — natively tested by
// test_unit_health.
#ifdef UNIT_TEST
  #include <cstdint>
  #include <cstddef>
  #include <cstdio>
#else
  #include <Arduino.h>
#endif

// Health / diagnostics snapshot returned by a sketch-running unit's
// CMD_GET_STATUS reply. Populated by UnitBus.cpp; mirrors the 8-byte layout
// documented in the v1 Unit.ino requestEvent().
struct UnitStatus {
  uint8_t  flags = 0;                    // bit0 moving, bit1 last-home-failed, bit2 hall-never-triggered
  uint8_t  mcusrAtBoot = 0;              // BORF / WDRF / EXTRF / PORF / JTRF snapshot
  uint8_t  lifetimeBrownoutCount = 0;    // saturating
  uint8_t  lifetimeWatchdogCount = 0;    // saturating
  uint16_t uptimeSeconds = 0;            // saturating
  uint8_t  badCommandCount = 0;          // saturating
  uint16_t lastHomingStepCount = 0;      // decoded from byte 7 * 16
};

// Status-flag bit positions (must match the v1 Unit.ino requestEvent()).
#define UNIT_FLAG_MOVING            (1 << 0)
#define UNIT_FLAG_LAST_HOME_FAILED  (1 << 1)
#define UNIT_FLAG_HALL_NEVER        (1 << 2)

// Everything the master knows about one unit slot — the per-unit facts the
// DisplaySnapshot carries (POD, ~24 B/slot). fwStatus keeps v1's /settings
// vocabulary (0 ok / 1 outdated / 2 unknown); slice A has no bundled unit
// hex to compare against, so UnitBus reports 2 for every readable version —
// slice C's reflash brings the real comparison target.
struct UnitFacts {
  uint8_t state = 0;         // 0 silent / 1 sketch / 2 bootloader
  uint8_t fwStatus = 2;      // 0 ok / 1 outdated / 2 unknown
  char version[9] = {0};     // git short-rev; "" when the read failed
  bool statusValid = false;  // status below holds a real CMD_GET_STATUS read
  UnitStatus status{};
  // Calibration offset (#204): probe-time CMD_GET_OFFSET truth, patched in
  // place by displayTask after a successful SET_OFFSET. offsetValid stays
  // false for silent/bootloader units and firmware predating the opcode
  // (v1 #32), and is dropped when a bootloader reboot invalidates reads.
  int16_t offset = 0;
  bool offsetValid = false;
};

// fwStatus from a unit's reported rev vs the build's bundled unit rev
// (#205; v1 compared with strncmp(..., 8) against an 8-char + NUL cache —
// same semantics here so a "-dirty" sidecar still matches its prefix).
// Unreadable version or no bundle → 2 (unknown), never a false OUTDATED.
inline uint8_t unitFwStatusFromRev(const char* version,
                                   const char* bundledRev) {
  if (version == nullptr || version[0] == '\0') return 2;
  if (bundledRev == nullptr || bundledRev[0] == '\0') return 2;
  for (int i = 0; i < 8; i++) {
    if (version[i] != bundledRev[i]) return 1;
    if (version[i] == '\0') break;  // both ended together — match
  }
  return 0;
}

// A unit is "faulty" when its last home failed, its hall sensor never fired
// during that home, or it has accrued any lifetime brownout/watchdog reset.
// badCommandCount is surfaced in the UI/attrs but deliberately NOT counted as
// a fault — a stray malformed I2C receive is not a hardware problem (#45/#137).
inline bool unitStatusIsFaulty(const UnitStatus& s) {
  if (s.flags & UNIT_FLAG_LAST_HOME_FAILED) return true;
  if (s.flags & UNIT_FLAG_HALL_NEVER)       return true;
  if (s.lifetimeBrownoutCount > 0)          return true;
  if (s.lifetimeWatchdogCount > 0)          return true;
  return false;
}

// Number of units we hold a valid status for AND that are faulty. Units we
// never read (statusValid false: silent, in bootloader, or old firmware
// without CMD_GET_STATUS) can't be assessed and are not counted — the count is
// the HA alerting signal, so it must not fire on units we simply can't see.
inline int computeFaultyUnitCount(const UnitFacts* units, int n) {
  int count = 0;
  for (int i = 0; i < n; i++) {
    if (units[i].statusValid && unitStatusIsFaulty(units[i].status)) count++;
  }
  return count;
}

// Worst case (16 valid units, all counters saturated) is ~2 KB — same cap
// and truncation contract as v1.
#define UNIT_HEALTH_JSON_CAP 2048

// Append-with-guard: bail the moment the buffer is full so buf+o never runs
// past the end. The caller rejects any payload whose returned length >= cap.
#define UNIT_HEALTH_APPEND(...) do { \
    if (o >= cap) return o; \
    o += (size_t)snprintf(buf + o, cap - o, __VA_ARGS__); \
  } while (0)

// Builds the per-unit health JSON (v1 wire shape, one wire contract for
// /units/health across both firmware generations):
//   {"width":W,"faulty":F,"units":[ {per-slot} , ... ]}
// One slot per display column 0..width-1 (so a unit stuck in bootloader or a
// silent gap is visible, not hidden by a headline count). Each slot always
// carries index/addr/state; the health fields are emitted only for a unit we
// actually read (statusValid):
//   {"i":0,"a":1,"st":1,"v":1,"fw":2,"rev":"abc12345","up":1200,"br":0,"wd":0,"bc":0,"mc":54,"fl":0,"hs":720}
//   {"i":5,"a":6,"st":2,"v":0}   (bootloader/silent/old-fw: nothing to report)
// `base` is SFP_I2C_ADDRESS_BASE so addr == base + index. The version string
// is emitted raw — UnitBus's readUnitVersion rejects `"` and `\` at the I2C
// boundary (v1 #140), so it can never break the JSON.
inline size_t buildUnitHealthJson(char* buf, size_t cap, const UnitFacts* units,
                                  int width, int faulty, int base) {
  size_t o = 0;
  UNIT_HEALTH_APPEND("{\"width\":%d,\"faulty\":%d,\"units\":[", width, faulty);
  for (int i = 0; i < width; i++) {
    const UnitFacts& u = units[i];
    UNIT_HEALTH_APPEND("%s{\"i\":%d,\"a\":%d,\"st\":%d,\"v\":%d",
                       i == 0 ? "" : ",", i, base + i, u.state,
                       u.statusValid ? 1 : 0);
    if (u.statusValid) {
      const UnitStatus& s = u.status;
      UNIT_HEALTH_APPEND(",\"fw\":%d,\"rev\":\"%s\",\"up\":%u,\"br\":%u,\"wd\":%u,\"bc\":%u,\"mc\":%u,\"fl\":%u,\"hs\":%u",
                         u.fwStatus, u.version, (unsigned)s.uptimeSeconds,
                         (unsigned)s.lifetimeBrownoutCount,
                         (unsigned)s.lifetimeWatchdogCount,
                         (unsigned)s.badCommandCount, (unsigned)s.mcusrAtBoot,
                         (unsigned)s.flags, (unsigned)s.lastHomingStepCount);
    }
    UNIT_HEALTH_APPEND("}");
  }
  UNIT_HEALTH_APPEND("]}");
  return o;
}
