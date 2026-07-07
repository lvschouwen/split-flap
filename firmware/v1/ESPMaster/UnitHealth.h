#pragma once

// Pure unit-health logic (#45 web card, #137 MQTT telemetry): the UnitStatus
// decode target, the "is this unit faulty" predicate, the faulty-unit count,
// and the shared per-unit JSON assembly consumed by BOTH the /units/health web
// endpoint AND the MQTT json_attributes_topic. No networking and no Wire — the
// whole header is exercised by `pio test -e native` (test_unit_health).
//
// Format strings are plain (small, built once per ~60 s telemetry tick into a
// static buffer) — no PROGMEM gymnastics here, unlike the 512-byte MQTT
// discovery payloads.
#ifdef UNIT_TEST
  #include <cstdint>
  #include <cstddef>
  #include <cstdio>
#else
  #include <Arduino.h>
#endif

// Health / diagnostics snapshot returned by a sketch-running unit's
// CMD_GET_STATUS reply. Populated by readUnitStatus() in
// ServiceFlapFunctions.ino; mirrors the 8-byte layout documented in Unit.ino's
// requestEvent().
struct UnitStatus {
  uint8_t  flags;                    // bit0 moving, bit1 last-home-failed, bit2 hall-never-triggered
  uint8_t  mcusrAtBoot;              // BORF / WDRF / EXTRF / PORF / JTRF snapshot
  uint8_t  lifetimeBrownoutCount;    // saturating
  uint8_t  lifetimeWatchdogCount;    // saturating
  uint16_t uptimeSeconds;            // saturating
  uint8_t  badCommandCount;          // saturating
  uint16_t lastHomingStepCount;      // decoded from byte 7 * 16
};

// Status-flag bit positions (must match Unit.ino requestEvent()).
#define UNIT_FLAG_MOVING            (1 << 0)
#define UNIT_FLAG_LAST_HOME_FAILED  (1 << 1)
#define UNIT_FLAG_HALL_NEVER        (1 << 2)

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
// never read (valid[i] == false: silent, in bootloader, or old firmware
// without CMD_GET_STATUS) can't be assessed and are not counted — the count is
// the HA alerting signal, so it must not fire on units we simply can't see.
inline int computeFaultyUnitCount(const UnitStatus* health, const bool* valid, int n) {
  int count = 0;
  for (int i = 0; i < n; i++) {
    if (valid[i] && unitStatusIsFaulty(health[i])) count++;
  }
  return count;
}

// Append-with-guard: bail the moment the buffer is full so buf+o never runs
// past the end. The caller rejects any payload whose returned length >= cap.
#define UNIT_HEALTH_APPEND(...) do { \
    if (o >= cap) return o; \
    o += (size_t)snprintf(buf + o, cap - o, __VA_ARGS__); \
  } while (0)

// Builds the shared per-unit health JSON:
//   {"width":W,"faulty":F,"units":[ {per-slot} , ... ]}
// One slot per display column 0..width-1 (so a unit stuck in bootloader or a
// silent gap is visible, not hidden by a headline count — the "16/16 but 10
// flap" UX gap). Each slot always carries index/addr/state; the health fields
// are emitted only for a unit we actually read (valid[i]):
//   {"i":0,"a":1,"st":1,"v":1,"fw":0,"up":1200,"br":0,"wd":0,"bc":0,"mc":54,"fl":0,"hs":720}
//   {"i":5,"a":6,"st":2,"v":0}   (bootloader/silent/old-fw: nothing to report)
// Keys are terse to keep the ESP-01 buffer small; the web UI decodes mc/fl/fw
// in JS. `states`/`fwStatus`/`health`/`valid` are all indexed by unit index
// (length >= width). `base` is I2C_ADDRESS_BASE so addr == base + index.
inline size_t buildUnitHealthJson(char* buf, size_t cap, const UnitStatus* health,
    const bool* valid, const int* states, const int* fwStatus,
    int width, int faulty, int base) {
  size_t o = 0;
  UNIT_HEALTH_APPEND("{\"width\":%d,\"faulty\":%d,\"units\":[", width, faulty);
  for (int i = 0; i < width; i++) {
    UNIT_HEALTH_APPEND("%s{\"i\":%d,\"a\":%d,\"st\":%d,\"v\":%d",
                       i == 0 ? "" : ",", i, base + i, states[i], valid[i] ? 1 : 0);
    if (valid[i]) {
      const UnitStatus& s = health[i];
      UNIT_HEALTH_APPEND(",\"fw\":%d,\"up\":%u,\"br\":%u,\"wd\":%u,\"bc\":%u,\"mc\":%u,\"fl\":%u,\"hs\":%u",
                         fwStatus[i], (unsigned)s.uptimeSeconds, (unsigned)s.lifetimeBrownoutCount,
                         (unsigned)s.lifetimeWatchdogCount, (unsigned)s.badCommandCount,
                         (unsigned)s.mcusrAtBoot, (unsigned)s.flags, (unsigned)s.lastHomingStepCount);
    }
    UNIT_HEALTH_APPEND("}");
  }
  UNIT_HEALTH_APPEND("]}");
  return o;
}
