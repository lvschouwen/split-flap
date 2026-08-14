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
#include "UnitVitals.h"  // shared supply-Vcc/ram/cmd-pos diag packet (#306)
#include "UnitExtDiag.h"  // shared new-measurement diag packet (#365)
#include "UnitLifetime.h"  // shared across-power-cycle health packet (#406)
#include "UnitWireContract.h"  // shared core read/write wire formats (#405)

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
// Homed since boot (#309). With bit 0 (moving) it gives the boot-home state:
// the unit boots UNHOMED and homes on the first trigger, so a curl can see a
// row still waiting out its staggered self-home.
#define UNIT_FLAG_HOMED             (1 << 5)

// Boot-home state (#309) decoded from the status flags for the "hs2" API key:
//   0 unhomed  — not homed yet, not moving (waiting for a trigger)
//   1 homing   — not homed yet, moving (first calibrate in progress)
//   2 homed    — has homed at least once since boot
inline uint8_t unitBootHomeState(uint8_t flags) {
  if (flags & UNIT_FLAG_HOMED) return 2;
  return (flags & UNIT_FLAG_MOVING) ? 1 : 0;
}

// Reboot edge-detect state (#368): last-seen uptime/brownout/watchdog triple
// so heartbeatTick can log a unit reboot once, the same place #322 logs
// health transitions. Detection logic (unitRebootDetect) lives in
// UnitEventLog.h; the POD lives here so the copied FollowerEsp01 tree needs
// no master-only header. Inert in the FollowerEsp01 copy.
struct UnitRebootWatch {
  uint16_t lastUptime = 0;
  uint8_t  lastBrownout = 0;
  uint8_t  lastWatchdog = 0;
  bool     primed = false;
};

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
  // Wire contract the unit reports (#405), read from GET_VERSION alongside
  // the rev. protocolKnown separates "definitively a different contract" from
  // "could not read it at all" — only the first justifies force-flashing, or
  // a unit we merely cannot read would reflash itself at every power-up
  // (the v1 #114 guard). Compared for EQUALITY only: neither this nor the rev
  // (a hash) can tell newer from older, and different always means reflash.
  uint8_t protocolVersion = 0;
  bool protocolKnown = false;
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
  // Master-side only (#322): baseline of driftEvents already reflected on the
  // operator log, so a NEW drift/self-correction (#263, otherwise silent) can
  // be logged once. -1 until the first valid diag read this probe epoch; a
  // probe rescan re-zeroes the whole struct (re-baseline), but a transient
  // diag-read failure early-returns without touching it, so a poll gap can't
  // drop a drift log. Inert in the FollowerEsp01 copy. Policy in DriftLogPolicy.h.
  int16_t driftEventsBaseline = -1;
  // Supply-Vcc / free-RAM / commanded-position diagnostics (#306):
  // probe/health-poll CMD_GET_VITALS truth, same lifecycle as the odometer
  // (checksum-rejected replies from pre-vitals firmware leave vitalsValid
  // false — the "diagV2" gate in the spec).
  UnitVitals vitals{};
  bool vitalsValid = false;
  // New-measurement diagnostics (#365): probe/health-poll CMD_GET_EXT_DIAG
  // truth, same checksum-rejected-on-old-firmware lifecycle as vitals/odometer.
  UnitExtDiag extDiag{};
  bool extDiagValid = false;
  // Across-power-cycle health (#406): probe/health-poll CMD_GET_LIFETIME
  // truth, same checksum-rejected-on-old-firmware lifecycle as ext-diag. The
  // distinction from extDiag is the point — that one is since-boot and every
  // reboot forgets it, this one is what the unit's EEPROM remembers.
  UnitLifetimeFacts lifetime{};
  bool lifetimeValid = false;
  // Heartbeat freshness (#310), maintained by displayTask's scheduled poll.
  // lastSeenMs is millis() at the last good CMD_GET_STATUS read; misses is the
  // consecutive-miss counter (NACK/checksum/timeout increments, a good read
  // resets, saturating); stale latches once misses >= HEARTBEAT_MISS_THRESHOLD
  // — a unit that fell off the bus. Only tracked for sketch slots (state 1);
  // gaps reset to 0/false so an empty column never reads as "lost".
  uint32_t lastSeenMs = 0;
  uint8_t  misses = 0;
  bool     stale = false;
  // displayed==intended verdict (#264), stamped by displayApplyUnitFacts at
  // the moment the diag was polled — phys and the standing frame are only
  // coherent at that instant (#267: render-time comparison produced phantom
  // mismatches from stale phys vs newer frames).
  bool mismatch = false;
  // Master-side only (#322): last-logged unit-health condition mask
  // (UNIT_EVT_* in UnitEventLog.h) so an onset/recovery of home-failed /
  // hall-never / stale / mismatch / low-Vcc (#366) is logged once, not folded
  // silently into /units/health JSON. Same probe-epoch lifecycle as
  // driftEventsBaseline; inert in the FollowerEsp01 copy.
  uint8_t healthEventState = 0;
  // Per-unit I2C reliability attribution (#367): cumulative count of failed
  // transactions charged to THIS unit's address (saturating), and millis() of
  // the most recent one (0 = none since boot). The global busErrCount (#245)
  // can't tell WHICH unit degraded — this can, which is the instrument the
  // 400 kHz bus bump (#375) is validated against. Master-side mirror of the
  // UnitBus.cpp static counters, refreshed on each health poll; lifetime (never
  // reset by a probe rescan — a reliability signal, unlike the re-baselined
  // masks above). Inert in the FollowerEsp01 copy.
  uint16_t i2cErrors = 0;
  uint32_t lastErrorMs = 0;
  // Reboot edge-detect state (#368): last-seen uptime/brownout/watchdog
  // triple so heartbeatTick can log a unit reboot once, the same place #322
  // logs health transitions. Policy in UnitEventLog.h.
  UnitRebootWatch rebootWatch{};
};

// May we drive this unit at all (#405)? A sketch-running unit that reports a
// contract we do not speak is left strictly alone: no renders, no status
// polls, no calibration. It stays visible in /units/health as a fault and
// stays a reflash target, so the operator can always converge it — but we
// never guess at a protocol we have no code for.
inline bool unitDrivable(const UnitFacts& u) {
  return u.state == 1 &&
         !(u.protocolKnown && !unitProtocolSupported(u.protocolVersion));
}

// Saturating increment for the per-unit I2C error counter (#367). Pins at
// 0xFFFF instead of wrapping to 0 — a wrapped counter would read as "healthy".
inline uint16_t unitErrBump(uint16_t prev) {
  return prev >= 0xFFFF ? 0xFFFF : (uint16_t)(prev + 1);
}

// driftFlags bit positions (must match the unit's UnitDrift.h encode).
#define UNIT_DRIFT_FLAG_PENDING        (1 << 0)
#define UNIT_DRIFT_FLAG_POSITION_KNOWN (1 << 1)

// One rev comparator, shared by the bundle rev and every equivalence entry so
// the two can never be graded by different rules. v1 semantics: compare the
// first 8 chars, so a "-dirty" sidecar still matches its prefix. A ',' or ' '
// in the candidate reads as end-of-string — a rev contains neither, and that
// is what lets the same function walk a comma-separated (and possibly
// human-padded) list without copying entries out of it.
inline bool unitRevMatches(const char* version, const char* rev) {
  for (int i = 0; i < 8; i++) {
    char r = (rev[i] == ',' || rev[i] == ' ') ? '\0' : rev[i];
    if (version[i] != r) return false;
    if (version[i] == '\0') break;  // both ended together — match
  }
  return true;
}

// fwStatus from a unit's reported rev vs the build's bundled unit rev
// (#205). Unreadable version or no bundle → 2 (unknown), never a false
// OUTDATED.
//
// equivalentRevs (#440) is an optional comma-separated list of revs PROVEN to
// build byte-identical machine code to the bundle — a unit reporting one of
// them is running our code and is current. It exists because the reported rev
// identifies the COMMIT a unit was built at, not the CODE it is running: a
// comment-only edit moves the rev and nothing else, and the resulting
// permanent OUTDATED both cries wolf and (via ReflashPlan) makes every unit a
// reflash target. The list widens what counts as current and nothing more —
// an unproven rev still reads OUTDATED, and an unreadable one still reads
// UNKNOWN. The proof and the list's self-invalidation live in
// flashing/flasher/make_manifest.py.
inline uint8_t unitFwStatusFromRev(const char* version,
                                   const char* bundledRev,
                                   const char* equivalentRevs = nullptr) {
  if (version == nullptr || version[0] == '\0') return 2;
  if (bundledRev == nullptr || bundledRev[0] == '\0') return 2;
  if (unitRevMatches(version, bundledRev)) return 0;
  if (equivalentRevs != nullptr) {
    const char* p = equivalentRevs;
    while (*p != '\0') {
      while (*p == ',' || *p == ' ') p++;   // skip separators and padding
      if (*p == '\0') break;
      if (unitRevMatches(version, p)) return 0;
      while (*p != '\0' && *p != ',') p++;  // advance to the next entry
    }
  }
  return 1;
}

// Fleet-wide supply floor (#306/#366): the lowest since-boot vccMin any valid
// unit reports, or 0 when none report vitals (all pre-vitals firmware). The
// brownout smoking gun, surfaced as the /units/health headline "vccMin" and the
// HA vccMin sensor. vccMin==0 is the per-unit "no reading" sentinel and never
// participates. Pure so both surfaces share one definition (natively tested).
inline uint16_t unitFleetVccMin(const UnitFacts* units, int width) {
  uint16_t lo = 0xFFFF;
  for (int i = 0; i < width; i++) {
    const UnitVitals& vt = units[i].vitals;
    if (units[i].vitalsValid && vt.vccMin_mV != 0 && vt.vccMin_mV < lo)
      lo = vt.vccMin_mV;
  }
  return lo == 0xFFFF ? 0 : lo;
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
// spliced wear object (#231, ~45 B), the per-unit drift fields
// phys/de/dp/ds/mm (#263/#264, ~43 B/unit), the per-unit vitals block
// vcc/vmin/cp/ram (#306, ~44 B/unit + the headline "vccMin"), the per-unit
// heartbeat-freshness keys age/hs2/misses/stale (#310, ~44 B/unit), the
// per-unit I2C-reliability keys err/errAge (#367, ~32 B/unit) and the
// per-unit ext-diag keys se/sx/sag/he/dw/sb (#365, ~63 B/unit) and the
// per-unit lifetime keys hf/gates/sxl/stw0/str0/stw1/str1 (#406, ~60 B/unit
// worst case — each rides an emit-when-nonzero guard, so a fresh unit adds
// nothing) and the per-unit idle-hall keys fr/frd (#460, ~18 B/unit, same
// guard) so a full display can't push the payload into the headline-only
// fallback. The #406 keys raised the ceiling over the prior 7168 (#365).
// test_unit_health pins the worst case + headroom (a full 16-unit payload
// with the wear + reflash splices).
#define UNIT_HEALTH_JSON_CAP 8192

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
                                  int width, int faulty, int base,
                                  uint32_t nowMs) {
  size_t o = 0;
  // Headline supply-Vcc floor (#306): the lowest since-boot vccMin any valid
  // unit has reported — the brownout smoking gun. Omitted when no unit reports
  // vitals (all pre-vitals firmware) so it never appears as a phantom 0.
  uint16_t vccMinAll = unitFleetVccMin(units, width);
  UNIT_HEALTH_APPEND("{\"width\":%d,\"faulty\":%d", width, faulty);
  if (vccMinAll != 0) {
    UNIT_HEALTH_APPEND(",\"vccMin\":%u", (unsigned)vccMinAll);
  }
  UNIT_HEALTH_APPEND(",\"units\":[");
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
        // Poll-time verdict stamped by displayApplyUnitFacts (#267) — this
        // layer only serializes it.
        if (u.mismatch) {
          UNIT_HEALTH_APPEND(",\"mm\":1");
        }
      }
    }
    if (u.vitalsValid) {
      // Supply-Vcc diagnostics (#306), own valid flag like "odo" — a unit can
      // report status but run pre-vitals firmware. vcc/vmin in mV, cp = last
      // commanded flap index, ram = since-boot min free SRAM bytes.
      const UnitVitals& vt = u.vitals;
      UNIT_HEALTH_APPEND(",\"vcc\":%u,\"vmin\":%u,\"cp\":%u,\"ram\":%u",
                         (unsigned)vt.vccNow_mV, (unsigned)vt.vccMin_mV,
                         (unsigned)vt.cmdPos, (unsigned)vt.freeRamMin);
    }
    if (u.extDiagValid) {
      // New-measurement diagnostics (#365), own valid flag like "odo"/"vcc" —
      // a unit can report status but run pre-ext-diag firmware. se/sx = last
      // and worst-seen home step excess, sag = min Vcc during last move, he =
      // hall edges seen in the last completed rev, dw = moves in the rolling
      // duty window, sb = status bits (bit0 stall/jam).
      const UnitExtDiag& e = u.extDiag;
      UNIT_HEALTH_APPEND(",\"se\":%u,\"sx\":%u,\"sag\":%u,\"he\":%u,\"dw\":%u,\"sb\":%u",
                         (unsigned)e.stepExcessLast, (unsigned)e.stepExcessMax,
                         (unsigned)e.vccSagLastMove, (unsigned)e.hallEdgesLastRev,
                         (unsigned)e.dutyWindow, (unsigned)e.statusBits);
    }
    if (u.protocolKnown) {
      // Wire contract the unit reports (#405). pmm marks one we do not speak:
      // that unit is deliberately not rendered to or polled, so without this
      // key it would look merely silent rather than deliberately untouched.
      UNIT_HEALTH_APPEND(",\"pv\":%u", (unsigned)u.protocolVersion);
      if (!unitProtocolSupported(u.protocolVersion)) {
        UNIT_HEALTH_APPEND(",\"pmm\":1");
      }
    }
    if (u.lifetimeValid) {
      // Across-power-cycle health (#406), own valid flag like "odo"/"vcc" —
      // a unit can report status but run pre-lifetime firmware. hf = lifetime
      // failed-homing count, gates = active UNIT_GATE_* bits, sxl = worst home
      // step excess ever seen (the "sx" above forgets at every reboot),
      // stw0/str0 = the unit's FIRST self-test hall window and steps/rev,
      // stw1/str1 = its most recent. The first/last pairs are the diagnosis:
      // "hall window 46 when new, 12 now" is the trajectory that unit 0x0f's
      // two-hour decline had nowhere to live.
      //
      // Each key is emitted only when non-zero, so a healthy unit that has
      // never failed a homing and never run a self-test stays lean — the
      // same discipline as misses/stale below.
      const UnitLifetimeFacts& lt = u.lifetime;
      if (lt.homeFailedCount) UNIT_HEALTH_APPEND(",\"hf\":%u", (unsigned)lt.homeFailedCount);
      if (lt.featureGates)    UNIT_HEALTH_APPEND(",\"gates\":%u", (unsigned)lt.featureGates);
      if (lt.stepExcessLifetimeMax) {
        UNIT_HEALTH_APPEND(",\"sxl\":%u", (unsigned)lt.stepExcessLifetimeMax);
      }
      if (lt.selfTestFirstHallWindow || lt.selfTestLastHallWindow) {
        UNIT_HEALTH_APPEND(",\"stw0\":%u,\"stw1\":%u",
                           (unsigned)lt.selfTestFirstHallWindow,
                           (unsigned)lt.selfTestLastHallWindow);
      }
      if (lt.selfTestFirstStepsPerRev || lt.selfTestLastStepsPerRev) {
        UNIT_HEALTH_APPEND(",\"str0\":%u,\"str1\":%u",
                           (unsigned)lt.selfTestFirstStepsPerRev,
                           (unsigned)lt.selfTestLastStepsPerRev);
      }
      // Idle hall check reporting on itself (#460). fr = futile re-homes this
      // boot (ones that measured no drift, so they proved the WINDOW model
      // wrong — the one false-positive class a re-home cannot fix); frd = the
      // check has disarmed itself here and is no longer protecting this unit.
      // frd is the unit's own verdict, not this side re-deriving it from fr
      // against a copy of the unit's limit — that duplication is what #458
      // was. Both ride the emit-when-nonzero guard, so an armed unit that has
      // never argued with its model stays silent.
      if (lt.idleHallFutileRehomes) {
        UNIT_HEALTH_APPEND(",\"fr\":%u", (unsigned)lt.idleHallFutileRehomes);
      }
      if (lt.idleHallStoodDown) UNIT_HEALTH_APPEND(",\"frd\":1");
    }
    if (u.state == 1) {
      // Heartbeat freshness (#310): age = ms since the last good scheduled
      // read, hs2 = boot-home state (0 unhomed / 1 homing / 2 homed, #309).
      // Gated on state==1, NOT statusValid: a lost unit's current read FAILED
      // (statusValid=false) yet is exactly when misses/stale must surface —
      // status.flags/lastSeenMs hold the last-known values. misses/stale ride
      // an emit-when-nonzero guard so a healthy unit stays lean; stale latches
      // once misses >= the threshold.
      UNIT_HEALTH_APPEND(",\"age\":%lu,\"hs2\":%u",
                         (unsigned long)(nowMs - u.lastSeenMs),
                         (unsigned)unitBootHomeState(u.status.flags));
      if (u.misses > 0) UNIT_HEALTH_APPEND(",\"misses\":%u", (unsigned)u.misses);
      if (u.stale)      UNIT_HEALTH_APPEND(",\"stale\":1");
      // Per-unit I2C reliability (#367): cumulative error count for this
      // address + ms since its last error, emit-when-nonzero so a clean unit
      // stays lean. errAge is only meaningful once an error has been charged.
      if (u.i2cErrors > 0) {
        UNIT_HEALTH_APPEND(",\"err\":%u,\"errAge\":%lu", (unsigned)u.i2cErrors,
                           (unsigned long)(nowMs - u.lastErrorMs));
      }
    }
    UNIT_HEALTH_APPEND("}");
  }
  UNIT_HEALTH_APPEND("]}");
  return o;
}
