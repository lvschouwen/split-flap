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
// Bit 3 stays reserved for the unit's earmarked stuck-drum use.
#define UNIT_FLAG_MOVING            (1 << 0)
#define UNIT_FLAG_LAST_HOME_FAILED  (1 << 1)
#define UNIT_FLAG_HALL_NEVER        (1 << 2)
// Address source is EEPROM-provisioned, not DIP (#215). Informational, never
// a fault — but twiboot only listens on the DIP-derived address, so a set bit
// on a unit whose DIP differs means over-I2C reflash cannot reach it.
#define UNIT_FLAG_ADDR_EEPROM       (1 << 4)

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
  // Revolution odometer (#231): probe-time CMD_GET_ODOMETER truth, same
  // lifecycle as offset. odometerValid stays false for silent/bootloader
  // units and firmware predating the opcode (checksum-rejected replies).
  uint32_t odometer = 0;
  bool odometerValid = false;
  // Drift diagnostics (#263/#264): probe/health-poll CMD_GET_DIAG truth.
  // physLetter is the unit's hall-corrected PHYSICAL letter estimate
  // (0xFF = position never synced); driftFlags bit0 = re-home pending,
  // bit1 = position known. diagValid follows the odometer's lifecycle.
  uint8_t physLetter = 0xFF;
  uint8_t driftFlags = 0;
  uint8_t driftEvents = 0;
  int8_t lastDriftSteps = 0;
  bool diagValid = false;
};

// driftFlags bit positions (must match the unit's UnitDrift.h encode).
#define UNIT_DRIFT_FLAG_PENDING        (1 << 0)
#define UNIT_DRIFT_FLAG_POSITION_KNOWN (1 << 1)

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

// Worst case (16 valid units, all counters saturated, 10-digit odometers,
// full drift blocks) measures ~3020 B — same truncation contract as v1, cap
// raised over v1's 2048 for the spliced reflash progress object (#205,
// ~70 B), the per-unit "ae" field (#215), the per-unit "odo" field and the
// spliced wear object (#231, ~45 B), and the per-unit drift fields
// phys/de/dp/ds/mm (#263/#264, ~43 B/unit) so a full display can't push the
// payload into the headline-only fallback. test_unit_health pins the worst
// case + headroom.
#define UNIT_HEALTH_JSON_CAP 3456

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
// `intended` (optional) is the last frame the master actually sent, one
// letter index per column — when present, a unit whose hall-corrected
// physical letter disagrees gets "mm":1 (#264). nullptr = no frame truth
// yet (boot, post-stop), so no mismatch judgments are emitted.
inline size_t buildUnitHealthJson(char* buf, size_t cap, const UnitFacts* units,
                                  int width, int faulty, int base,
                                  const uint8_t* intended = nullptr) {
  size_t o = 0;
  UNIT_HEALTH_APPEND("{\"width\":%d,\"faulty\":%d,\"units\":[", width, faulty);
  for (int i = 0; i < width; i++) {
    const UnitFacts& u = units[i];
    UNIT_HEALTH_APPEND("%s{\"i\":%d,\"a\":%d,\"st\":%d,\"v\":%d",
                       i == 0 ? "" : ",", i, base + i, u.state,
                       u.statusValid ? 1 : 0);
    if (u.statusValid) {
      const UnitStatus& s = u.status;
      UNIT_HEALTH_APPEND(",\"fw\":%d,\"rev\":\"%s\",\"up\":%u,\"br\":%u,\"wd\":%u,\"bc\":%u,\"mc\":%u,\"fl\":%u,\"hs\":%u,\"ae\":%u",
                         u.fwStatus, u.version, (unsigned)s.uptimeSeconds,
                         (unsigned)s.lifetimeBrownoutCount,
                         (unsigned)s.lifetimeWatchdogCount,
                         (unsigned)s.badCommandCount, (unsigned)s.mcusrAtBoot,
                         (unsigned)s.flags, (unsigned)s.lastHomingStepCount,
                         (s.flags & UNIT_FLAG_ADDR_EEPROM) ? 1u : 0u);
    }
    if (u.odometerValid) {
      // Rides its own valid flag, independent of statusValid — a unit can
      // report status but run pre-odometer firmware (#231).
      UNIT_HEALTH_APPEND(",\"odo\":%lu", (unsigned long)u.odometer);
    }
    if (u.diagValid) {
      // Drift block (#263/#264), own valid flag like "odo": event count +
      // last magnitude always; "dp" only while a re-home is pending; "phys"
      // only once the unit has synced to a hall edge; "mm" only when both
      // the physical estimate and the master's intended frame exist and
      // disagree.
      UNIT_HEALTH_APPEND(",\"de\":%u,\"ds\":%d", (unsigned)u.driftEvents,
                         (int)u.lastDriftSteps);
      if (u.driftFlags & UNIT_DRIFT_FLAG_PENDING) {
        UNIT_HEALTH_APPEND(",\"dp\":1");
      }
      bool physKnown = (u.driftFlags & UNIT_DRIFT_FLAG_POSITION_KNOWN) &&
                       u.physLetter != 0xFF;
      if (physKnown) {
        UNIT_HEALTH_APPEND(",\"phys\":%u", (unsigned)u.physLetter);
        // No mismatch judgment against a rotating drum: a health refresh
        // racing an in-flight move (or a self-test's own restore rotation)
        // would flag a spurious, self-resolving "mm" (cpp-review MEDIUM).
        bool moving =
            u.statusValid && (u.status.flags & UNIT_FLAG_MOVING);
        if (!moving && intended != nullptr && u.physLetter != intended[i]) {
          UNIT_HEALTH_APPEND(",\"mm\":1");
        }
      }
    }
    UNIT_HEALTH_APPEND("}");
  }
  UNIT_HEALTH_APPEND("]}");
  return o;
}
