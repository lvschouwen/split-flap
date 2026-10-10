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
#include "UnitBusRecord.h"  // what a unit remembers of its master's silences (#584)
#include "UnitWireContract.h"  // shared core read/write wire formats (#405)
#include "BootIntegrity.h"  // boot-section verdict vocabulary (#520)
#include "TwibootProtocol.h"  // TwibootIdentity (#541)

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
// A unit's lifetime reset counters as this master first read them (#502). The
// counters are history kept in the unit's EEPROM: a brownout months ago, or
// the one reset a firmware campaign leaves behind, is not a fault today. What
// is one is a counter that climbs while this master is watching. Held by the
// bus layer outside UnitFacts, because a probe rescan rebuilds the facts.
struct UnitResetBaseline {
  bool    valid = false;
  uint8_t brownout = 0;
  uint8_t watchdog = 0;
};

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
  // Link-health extension behind the ext-diag packet (#502): its own checksum,
  // so linkValid can be false while extDiagValid is true (a unit without the
  // extension). Same stale-clearing lifecycle as extDiag.
  UnitLinkStats link{};
  bool linkValid = false;
  // Across-power-cycle health (#406): probe/health-poll CMD_GET_LIFETIME
  // truth, same checksum-rejected-on-old-firmware lifecycle as ext-diag. The
  // distinction from extDiag is the point — that one is since-boot and every
  // reboot forgets it, this one is what the unit's EEPROM remembers.
  UnitLifetimeFacts lifetime{};
  bool lifetimeValid = false;
  // The silences the unit lived through (#584, UnitBusRecord.h): its EEPROM's
  // count of the times nobody addressed it, read with the diagnostics. Same
  // checksum-rejected-on-old-firmware lifecycle as lifetime.
  UnitBusRecord busRecord{};
  uint8_t busSilentNowMinutes = 0;
  bool busRecordValid = false;
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
  // Runtime-rescue twiboot exits since boot (#498, UnitRescuePolicy.h),
  // mirrored from the row master's rescue state: a unit found sitting in
  // twiboot was reset and never got as far as counting it.
  uint16_t rescueExits = 0;
  // Reboot edge-detect state (#368): last-seen uptime/brownout/watchdog
  // triple so heartbeatTick can log a unit reboot once, the same place #322
  // logs health transitions. Policy in UnitEventLog.h.
  UnitRebootWatch rebootWatch{};
  // A lifetime brownout/watchdog counter has climbed since this master first
  // read the unit (unitResetBaselineFold). Refreshed with every status read.
  bool resetSeen = false;
  // Boot-section integrity (#520): the verdict on this health poll's
  // GET_BOOT_INFO read and the CRC it judged. UNREAD when the read failed, so
  // a unit that stops answering never keeps an old verdict.
  uint8_t bootVerdict = BOOT_INTEGRITY_UNREAD;
  uint32_t bootCrc32 = 0;
  // What the bootloader said about itself (#541/#543), read by the bus scan
  // from a unit found sitting in it. UNREAD for every unit running its
  // application: there the unit's own boot report above is the source.
  TwibootIdentity bootloader{};
};

// Folds one status read into the unit's baseline; true when the unit has reset
// unexpectedly since the baseline was taken. A counter that reads lower than
// its baseline was cleared on the unit (EEPROM re-init), so the baseline
// follows it down.
inline bool unitResetBaselineFold(UnitResetBaseline& b, uint8_t brownout,
                                  uint8_t watchdog) {
  if (!b.valid) {
    b.valid = true;
    b.brownout = brownout;
    b.watchdog = watchdog;
    return false;
  }
  if (brownout < b.brownout) b.brownout = brownout;
  if (watchdog < b.watchdog) b.watchdog = watchdog;
  return brownout > b.brownout || watchdog > b.watchdog;
}

// Folds one EXT_DIAG_LINK_REPLY_LEN read into the slot (#502). The base packet
// and the link extension are validated independently: a unit without the
// extension (bus padding behind byte 10) keeps its base data and reports the
// link fields absent. Both flags are cleared first so a reply that stops
// validating never leaves a stale reading.
inline void unitFactsFoldExtDiag(UnitFacts& fact,
                                 const uint8_t buf[EXT_DIAG_LINK_REPLY_LEN]) {
  fact.extDiagValid = false;
  fact.linkValid = false;
  UnitExtDiag d;
  if (extDiagReadbackValid(buf, d)) {
    fact.extDiag = d;
    fact.extDiagValid = true;
  }
  UnitLinkStats l;
  if (extDiagLinkReadbackValid(buf + EXT_DIAG_REPLY_LEN, l)) {
    fact.link = l;
    fact.linkValid = true;
  }
}

// May we drive this unit at all (#405)? A sketch-running unit that reports a
// contract we do not speak is left strictly alone: no renders, no status
// polls, no calibration. It stays visible in /units/health as a fault and
// stays a reflash target, so the operator can always converge it — but we
// never guess at a protocol we have no code for.
inline bool unitDrivable(const UnitFacts& u) {
  return u.state == 1 &&
         !(u.protocolKnown && !unitProtocolSupported(u.protocolVersion));
}

// --- in-place fact patches -------------------------------------------------------
// A probe rewrites a unit's facts wholesale; these are the only mutations in
// between, each applied after the op that justifies it was verified.

inline void unitFactsApplyOffsetWrite(UnitFacts& u, int16_t value) {
  u.offset = value;
  u.offsetValid = true;
}

// A successful RESET_ODOMETER: the wear view must not show the old count
// until the next probe (#231).
inline void unitFactsApplyOdometerReset(UnitFacts& u) {
  u.odometer = 0;
  u.odometerValid = true;
}

// A verified SET_GATES (#409) — only after the read-back confirmed it, so
// this cannot invent a gate the unit did not accept.
inline void unitFactsApplyGatesWrite(UnitFacts& u, uint8_t gates) {
  u.lifetime.featureGates = gates;
}

// A unit sent into twiboot forgets nothing, but its row master must stop
// serving reads for it until the next probe confirms it is back in sketch.
inline void unitFactsInvalidateReads(UnitFacts& u) {
  u.offsetValid = false;
  u.statusValid = false;
  u.odometerValid = false;
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

// What a status read alone says is wrong: the last home failed, or the hall
// sensor never fired during it. The lifetime brownout/watchdog counters are
// NOT judged here — their absolute value is history; UnitFacts::resetSeen
// carries "it reset while we were watching". badCommandCount is surfaced in
// the UI/attrs but deliberately NOT counted as a fault — a stray malformed I2C
// receive is not a hardware problem (#45/#137).
inline bool unitStatusIsFaulty(const UnitStatus& s) {
  if (s.flags & UNIT_FLAG_LAST_HOME_FAILED) return true;
  if (s.flags & UNIT_FLAG_HALL_NEVER)       return true;
  return false;
}

// A unit the rescue found held in its bootloader (#542): its application
// reads are void, and it is a bootloader unit until a probe says otherwise.
// The slot is rebuilt as a scan would leave it; only the bus-side history,
// which is about the address and not the application, carries over.
inline void unitFactsBecomeBootloader(UnitFacts& u, const TwibootIdentity& id) {
  UnitFacts fresh{};
  fresh.state = 2;
  fresh.bootloader = id;
  fresh.i2cErrors = u.i2cErrors;
  fresh.lastErrorMs = u.lastErrorMs;
  fresh.rescueExits = u.rescueExits;
  u = fresh;
}

// A sketch unit that answered once and has since missed the heartbeat
// threshold (#310) — silent on the bus, wedged, or reset into twiboot. Only
// state 1 counts: heartbeatApply never tracks other slots.
inline bool unitIsLost(const UnitFacts& u) {
  return u.state == 1 && u.stale;
}

// The per-unit alerting predicate: lost, or a valid status that reports a
// fault. Units never read (statusValid false and not stale: empty, in
// bootloader, old firmware without CMD_GET_STATUS) can't be assessed and
// don't count. A lost unit's status is unreadable by definition, so it can't
// be gated on statusValid (#497: a unit dead for 12 h reported faulty 0).
inline bool unitIsFaultyOrLost(const UnitFacts& u) {
  if (unitIsLost(u)) return true;
  // Its own read, so its own gate: a bootloader that matches no known image
  // is the unit's last remote recovery path rotting (#520).
  if (u.bootVerdict == BOOT_INTEGRITY_CORRUPT) return true;
  // Held in its bootloader because the application keeps crashing (#542).
  if (u.state == 2 && twibootHeldForCrashing(u.bootloader)) return true;
  return u.statusValid && (unitStatusIsFaulty(u.status) || u.resetSeen);
}

// The HA units_faulty signal and the cluster ping's faulty key.
inline int computeFaultyUnitCount(const UnitFacts* units, int n) {
  int count = 0;
  for (int i = 0; i < n; i++) {
    if (unitIsFaultyOrLost(units[i])) count++;
  }
  return count;
}

// Lost units only. Unlike faulty (where a reset seen stays until this master
// reboots), this
// clears as soon as the unit answers again, so it can drive cluster degrade.
inline int computeLostUnitCount(const UnitFacts* units, int n) {
  int count = 0;
  for (int i = 0; i < n; i++) {
    if (unitIsLost(units[i])) count++;
  }
  return count;
}

// Hex fault bitmap for one row: bit i = unit at position i is faulty or lost
// (same predicate as computeFaultyUnitCount). Fixed width ceil(width/4) nibbles
// so the string length itself carries the row width; enough for a strip —
// per-unit detail is the member's own /units/health.
inline size_t unitFaultMaskHex(const UnitFacts* units, int width,
                                  char* buf, size_t cap) {
  if (width > 32) width = 32;  // uint32 bitmap; real displays are ≤16
  if (width <= 0) {            // no units, no mask (%0*X would still print "0")
    if (cap > 0) buf[0] = '\0';
    return 0;
  }
  uint32_t mask = 0;
  for (int i = 0; i < width; i++) {
    if (unitIsFaultyOrLost(units[i])) mask |= (1UL << i);
  }
  int nibbles = (width + 3) / 4;
  if (cap == 0) return 0;
  int n = snprintf(buf, cap, "%0*X", nibbles, (unsigned)mask);
  if (n < 0) {
    buf[0] = '\0';
    return 0;
  }
  return (size_t)n < cap ? (size_t)n : cap - 1;
}

// How large the facts document of a row can get, with the wear and the
// unit-update objects a board splices in behind the units. The two figures
// are the measured worst case — every key the serializer writes, every value
// at its widest (UnitFactsWidest.h) — and test_unit_facts_widest holds them
// to it from both sides: the document fits at every width, and no width
// leaves more than a few bytes over. A document that does not fit is not
// sent at all, so a key added to the serializer moves these.
#define UNIT_FACTS_DOC_BASE_BYTES     252
#define UNIT_FACTS_DOC_PER_UNIT_BYTES 679
#define UNIT_FACTS_DOC_CAP(width) \
  ((size_t)UNIT_FACTS_DOC_BASE_BYTES + (size_t)(width) * UNIT_FACTS_DOC_PER_UNIT_BYTES)
#define UNIT_HEALTH_JSON_CAP UNIT_FACTS_DOC_CAP(16)

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
      // rs: a lifetime reset counter climbed while this master was watching —
      // the part of br/wd that counts as a fault. Emitted only when set.
      if (u.resetSeen) UNIT_HEALTH_APPEND(",\"rs\":1");
    }
    if (u.odometerValid) {
      // Rides its own valid flag, independent of statusValid — a unit can
      // report status but run pre-odometer firmware (#231).
      UNIT_HEALTH_APPEND(",\"odo\":%lu", (unsigned long)u.odometer);
    }
    if (u.offsetValid) {
      // Calibration offset in steps, own valid flag like "odo": the value the
      // probe read, patched in place by a verified write.
      UNIT_HEALTH_APPEND(",\"ofs\":%d", (int)u.offset);
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
    if (u.linkValid) {
      // Link health (#502), own valid flag: the extension has its own checksum.
      // ut = full since-boot uptime in seconds (the status "up" saturates at
      // 65535), rx/tx = master writes received / reads answered by the unit
      // (wrapping u16 — compare deltas), dh = TWI register self-check re-inits
      // since boot.
      const UnitLinkStats& l = u.link;
      UNIT_HEALTH_APPEND(",\"ut\":%lu,\"rx\":%u,\"tx\":%u,\"dh\":%u",
                         (unsigned long)l.uptimeSeconds, (unsigned)l.rxFrames,
                         (unsigned)l.txReplies, (unsigned)l.deafHeals);
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
    if (u.busRecordValid) {
      // The unit's own record of its master's silences (#584). bsn = how many,
      // always there when the record was read, so "none" and "cannot say"
      // differ; the rest only when not zero: bsl/bsx = minutes of the latest
      // and the longest, bsr = restarts of its bus hardware, bsh = of those,
      // the ones contact followed, bss = restarts of the whole unit, bsf =
      // BUS_RECORD_FLAG_* of the latest, bsq = minutes of one going on now.
      const UnitBusRecord& br = u.busRecord;
      UNIT_HEALTH_APPEND(",\"bsn\":%u", (unsigned)br.silences);
      if (br.lastMinutes) UNIT_HEALTH_APPEND(",\"bsl\":%u", (unsigned)br.lastMinutes);
      if (br.longestMinutes) UNIT_HEALTH_APPEND(",\"bsx\":%u", (unsigned)br.longestMinutes);
      if (br.reinits) UNIT_HEALTH_APPEND(",\"bsr\":%u", (unsigned)br.reinits);
      if (br.reinitsHeard) UNIT_HEALTH_APPEND(",\"bsh\":%u", (unsigned)br.reinitsHeard);
      if (br.selfRestarts) UNIT_HEALTH_APPEND(",\"bss\":%u", (unsigned)br.selfRestarts);
      if (br.flags) UNIT_HEALTH_APPEND(",\"bsf\":%u", (unsigned)br.flags);
      if (u.busSilentNowMinutes) {
        UNIT_HEALTH_APPEND(",\"bsq\":%u", (unsigned)u.busSilentNowMinutes);
      }
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
      if (u.rescueExits > 0) {
        UNIT_HEALTH_APPEND(",\"rsx\":%u", (unsigned)u.rescueExits);
      }
    }
    if (u.bootVerdict != BOOT_INTEGRITY_UNREAD) {
      // Boot-section integrity (#520): bv = 1 the expected bootloader image,
      // 2 a known other image or update step, 3 matches nothing known (counts
      // as faulty). bcrc = the CRC the unit reported, only when it is not the
      // expected one.
      UNIT_HEALTH_APPEND(",\"bv\":%u", (unsigned)u.bootVerdict);
      if (u.bootVerdict != BOOT_INTEGRITY_OK) {
        UNIT_HEALTH_APPEND(",\"bcrc\":\"%08lx\"", (unsigned long)u.bootCrc32);
      }
    }
    if (u.state == 2 && u.bootloader.generation != TWIBOOT_GEN_UNREAD) {
      // A unit sitting in its bootloader (#541/#543): blv = 1 an image
      // without identity bytes, 255 not recognised, else the image's version;
      // blc = its capability bits; blk / blf = lock byte and low, high,
      // extended fuse, only when the chip served real ones.
      const TwibootIdentity& b = u.bootloader;
      UNIT_HEALTH_APPEND(",\"blv\":%u,\"blc\":%u", (unsigned)b.generation,
                         (unsigned)b.caps);
      if (b.fusesValid) {
        UNIT_HEALTH_APPEND(",\"blk\":\"%02x\",\"blf\":\"%02x%02x%02x\"",
                           (unsigned)b.lock, (unsigned)b.lfuse,
                           (unsigned)b.hfuse, (unsigned)b.efuse);
      }
      // blx = crash resets in a row the bootloader counted (#542), shown from
      // 2 up (1 is the intentional reset of any reflash); at 3 it holds the
      // unit, which counts as faulty.
      if (b.crashValid && b.crashCount >= TWIBOOT_CRASH_REPORT_FROM) {
        UNIT_HEALTH_APPEND(",\"blx\":%u", (unsigned)b.crashCount);
      }
    }
    UNIT_HEALTH_APPEND("}");
  }
  UNIT_HEALTH_APPEND("]}");
  return o;
}
