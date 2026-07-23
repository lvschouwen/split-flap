# Unit Ext-Diag (epic #365 unit-fw slice) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Surface the per-unit diagnostics the unit already transmits (Slice A, no reflash) and add a lean `GET_EXT_DIAG` packet for new-measurement signals (Slice B, one reflash), all in one branch/PR.

**Architecture:** Slice A is master-only: decode + log + HA for `GET_STATUS` (0x83) fields already in `UnitStatus` (reset-cause, reboot counts, uptime). Slice B adds unversioned opcode `GET_EXT_DIAG` (0x89), an 11-byte checksummed reply (`UnitExtDiag.h`, copied to all three trees), unit-side glue populating it, a master round-robin read folded into `unitBusPollHealthOne`, JSON/log/HA surfacing, and a bundled unit reflash.

**Tech Stack:** PlatformIO; C++ pure headers (native-tested via ArduinoFake/Unity); AVR (Nano unit, bench tier); ESP32-S3 (Master) + ESP8266 (FollowerEsp01); Python drift gates.

## Global Constraints

- **Copy policy:** `UnitExtDiag.h` is a byte-identical COPY in `firmware/v2/Unit/`, `firmware/v2/Master/`, `firmware/v2/FollowerEsp01/`. Fix bugs in all three. Registered in `tests/test_copied_headers.py` (`UNIT_HEALTH_JSON`-adjacent manifest list) or the copy set fails CI.
- **Unversioned opcode:** `GET_EXT_DIAG` = `0x89`, no wire version byte. Backward-compat = masked-XOR checksum degrade (`diagExt=false` on un-flashed units). A future format change takes a new opcode (`0x8A`).
- **Checksum mask:** `EXT_DIAG_REPLY_CHECKSUM_MASK = 0x93` — distinct from existing masks (0x3C vitals/odo-slot, 0xA5 odo, 0xB7 drift, 0x5C selftest).
- **No new EEPROM slots** on the unit. All Slice-B signals hang off existing hooks.
- **Wire ISR discipline:** nothing new runs in `receiveLetter`/`requestEvent` beyond setting a `pending*` flag and streaming a pre-built buffer. Encoding happens loop-side.
- **Bundle flow (load-bearing):** if Unit fw changes → rebuild Unit clean → `flashing/flasher/make_manifest.py stage` → rebuild Master + FollowerEsp01 → **separate** artifact commit (never amend the bundle into a code commit).
- **Build gates before commit:** `pio run` in each touched project dir, `pio test -e native` (Unit + Master + FollowerEsp01), `python -m pytest tests/`. Editing v2 source auto-triggers that project's native suite via the PostToolUse hook.
- **Risk-tiered review:** cpp-reviewer over the combined branch diff before PR (cluster-wire/flash not touched here, but I2C-wire IS — review ALWAYS).

---

## File Structure

**Slice A (master-only):**
- Modify `firmware/v2/Master/UnitEventLog.h` — add reset-cause decode + reboot-detect pure helpers.
- Modify `firmware/v2/Master/UnitHealth.h` — add `UnitRebootWatch` state to `UnitFacts`.
- Modify `firmware/v2/Master/DisplayTask.cpp` — emit reset/reboot log lines in `heartbeatTick`.
- Modify `firmware/v2/Master/MqttHelpers.h` + `MqttService.cpp` — reset-cause + reboot-count HA sensors.
- Modify `firmware/v2/Master/test/test_unit_event_log/` + `test/test_mqtt_helpers/`.

**Slice B (unit reflash):**
- Create `firmware/v2/Unit/UnitExtDiag.h` (+ copies in Master, FollowerEsp01).
- Modify `firmware/v2/shared/SplitFlapProtocol.h` — `SFP_CMD_GET_EXT_DIAG 0x89`.
- Modify `firmware/v2/Unit/Unit.ino` (globals + reply buf), `UnitI2CProtocol.ino` (dispatch + requestEvent), `UnitMotion.ino` (excess/sag/hall/stall glue).
- Modify `firmware/v2/Master/UnitBus.cpp` (read + refresh), `UnitHealth.h` (`UnitFacts` ext-diag fields + JSON keys).
- Modify `firmware/v2/Master/DisplayTask.cpp` (jam/drag/hall-anomaly log edges), `UnitEventLog.h` (new event bits).
- Modify `firmware/v2/Master/MqttHelpers.h` + `MqttService.cpp` (jam binary_sensor, excess sensor).
- Modify `firmware/v2/FollowerEsp01/` (header copy + `/units/health` keys + api-index if surfaced).
- New native tests: `firmware/v2/Unit/test/test_ext_diag/`, `firmware/v2/Master/test/test_ext_diag/`.
- Modify `tests/test_copied_headers.py`, buffer-cap test in `test_unit_health`.

---

## SLICE A — master-only surfacing (no reflash)

### Task A1: Reset-cause decode helper (pure)

**Files:**
- Modify: `firmware/v2/Master/UnitEventLog.h`
- Test: `firmware/v2/Master/test/test_unit_event_log/test_unit_event_log.cpp`

**Interfaces:**
- Produces: `enum UnitResetCause`; `UnitResetCause unitResetCauseDecode(uint8_t mcusr)`; `const char* unitResetCauseName(UnitResetCause)`.

- [ ] **Step 1: Write the failing test** — append to `test_unit_event_log.cpp`:

```cpp
void test_reset_cause_priority_brownout_over_watchdog() {
    // BORF (bit2) + WDRF (bit3) both set -> brownout wins (most actionable).
    TEST_ASSERT_EQUAL(RESET_BROWNOUT, unitResetCauseDecode((1<<2) | (1<<3)));
}
void test_reset_cause_each_flag() {
    TEST_ASSERT_EQUAL(RESET_WATCHDOG, unitResetCauseDecode(1<<3));
    TEST_ASSERT_EQUAL(RESET_EXTERNAL, unitResetCauseDecode(1<<1));
    TEST_ASSERT_EQUAL(RESET_POWER_ON, unitResetCauseDecode(1<<0));
    TEST_ASSERT_EQUAL(RESET_UNKNOWN,  unitResetCauseDecode(0));
}
void test_reset_cause_name_nonnull() {
    TEST_ASSERT_EQUAL_STRING("brownout", unitResetCauseName(RESET_BROWNOUT));
    TEST_ASSERT_EQUAL_STRING("power-on", unitResetCauseName(RESET_POWER_ON));
}
```

Register the three in the file's `main()` with `RUN_TEST(...)`.

- [ ] **Step 2: Run test to verify it fails**

Run: `cd firmware/v2/Master && pio test -e native -f test_unit_event_log`
Expected: FAIL — `unitResetCauseDecode` not declared.

- [ ] **Step 3: Add the helper to `UnitEventLog.h`** (after the `#define UNIT_EVT_*` block):

```cpp
// Unit reset-cause decoded from the MCUSR snapshot GET_STATUS byte 1 carries
// (#368). Priority is most-actionable-first: a brownout (BORF) is the operator
// signal that matters, so it wins over a co-set watchdog/power-on flag.
enum UnitResetCause {
  RESET_UNKNOWN = 0, RESET_POWER_ON, RESET_EXTERNAL, RESET_BROWNOUT, RESET_WATCHDOG
};
inline UnitResetCause unitResetCauseDecode(uint8_t mcusr) {
  if (mcusr & (1 << 2)) return RESET_BROWNOUT;  // BORF
  if (mcusr & (1 << 3)) return RESET_WATCHDOG;  // WDRF
  if (mcusr & (1 << 1)) return RESET_EXTERNAL;  // EXTRF
  if (mcusr & (1 << 0)) return RESET_POWER_ON;  // PORF
  return RESET_UNKNOWN;
}
inline const char* unitResetCauseName(UnitResetCause c) {
  switch (c) {
    case RESET_BROWNOUT: return "brownout";
    case RESET_WATCHDOG: return "watchdog";
    case RESET_EXTERNAL: return "external";
    case RESET_POWER_ON: return "power-on";
    default:             return "unknown";
  }
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd firmware/v2/Master && pio test -e native -f test_unit_event_log`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add firmware/v2/Master/UnitEventLog.h firmware/v2/Master/test/test_unit_event_log/
git commit -m "feat(#368): reset-cause decode helper for GET_STATUS mcusr byte"
```

### Task A2: Reboot-detect edge helper (pure)

**Files:**
- Modify: `firmware/v2/Master/UnitEventLog.h`
- Test: `firmware/v2/Master/test/test_unit_event_log/test_unit_event_log.cpp`

**Interfaces:**
- Produces: `struct UnitRebootWatch { uint16_t lastUptime; uint8_t lastBrownout; uint8_t lastWatchdog; bool primed; }`; `bool unitRebootDetect(UnitRebootWatch&, uint16_t uptime, uint8_t brownout, uint8_t watchdog)`.

- [ ] **Step 1: Write the failing test:**

```cpp
void test_reboot_detect_primes_silent() {
    UnitRebootWatch w{};
    // First observation only primes — never logs a phantom reboot.
    TEST_ASSERT_FALSE(unitRebootDetect(w, 100, 0, 0));
}
void test_reboot_detect_uptime_drop() {
    UnitRebootWatch w{};
    unitRebootDetect(w, 500, 0, 0);           // prime
    TEST_ASSERT_TRUE(unitRebootDetect(w, 12, 0, 0));   // uptime fell -> reboot
    TEST_ASSERT_FALSE(unitRebootDetect(w, 30, 0, 0));  // climbing -> no event
}
void test_reboot_detect_counter_climb() {
    UnitRebootWatch w{};
    unitRebootDetect(w, 500, 1, 0);           // prime
    // Fast reboot: uptime may not have visibly dropped but brownout count rose.
    TEST_ASSERT_TRUE(unitRebootDetect(w, 505, 2, 0));
}
```

Register all three in `main()`.

- [ ] **Step 2: Run test to verify it fails**

Run: `cd firmware/v2/Master && pio test -e native -f test_unit_event_log`
Expected: FAIL — `UnitRebootWatch` not declared.

- [ ] **Step 3: Add to `UnitEventLog.h`:**

```cpp
// Reboot edge (#368): a unit that browns out / watchdog-resets just re-homes
// and looks healthy. GET_STATUS carries uptime + lifetime brownout/watchdog
// counts; a reboot shows as uptime falling OR either count climbing. State
// lives per-unit in UnitFacts. First observation only primes (no phantom).
struct UnitRebootWatch {
  uint16_t lastUptime = 0;
  uint8_t  lastBrownout = 0;
  uint8_t  lastWatchdog = 0;
  bool     primed = false;
};
inline bool unitRebootDetect(UnitRebootWatch& w, uint16_t uptime,
                             uint8_t brownout, uint8_t watchdog) {
  if (!w.primed) {
    w.lastUptime = uptime; w.lastBrownout = brownout;
    w.lastWatchdog = watchdog; w.primed = true;
    return false;
  }
  bool rebooted = uptime < w.lastUptime ||
                  brownout != w.lastBrownout || watchdog != w.lastWatchdog;
  w.lastUptime = uptime; w.lastBrownout = brownout; w.lastWatchdog = watchdog;
  return rebooted;
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd firmware/v2/Master && pio test -e native -f test_unit_event_log`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add firmware/v2/Master/UnitEventLog.h firmware/v2/Master/test/test_unit_event_log/
git commit -m "feat(#368): reboot-detect edge helper (uptime drop / counter climb)"
```

### Task A3: Wire reboot logging into heartbeatTick

**Files:**
- Modify: `firmware/v2/Master/UnitHealth.h:61` (`UnitFacts` struct) — add `UnitRebootWatch rebootWatch;`
- Modify: `firmware/v2/Master/DisplayTask.cpp` (`heartbeatTick`, near the existing #322 event-log emit around line 197+)

**Interfaces:**
- Consumes: `unitResetCauseDecode`, `unitResetCauseName`, `unitRebootDetect` (A1/A2); `UnitFacts.status` (`mcusrAtBoot`, `uptimeSeconds`, `lifetimeBrownoutCount`, `lifetimeWatchdogCount`), `UnitFacts.statusValid`.

- [ ] **Step 1: Add reboot-watch state to `UnitFacts`** — in `UnitHealth.h`, inside `struct UnitFacts`, next to the event-log mask field:

```cpp
  UnitRebootWatch rebootWatch{};   // #368 per-unit reboot edge state
```

- [ ] **Step 2: Emit the log line in `heartbeatTick`** — where the #322 per-unit event emit runs (only for `statusValid` units), add:

```cpp
    // #368: log a unit reboot the same place #322 logs health transitions —
    // only when the status read is fresh, so a silent unit never fabricates one.
    if (busFacts[u].statusValid) {
      const UnitStatus& s = busFacts[u].status;
      if (unitRebootDetect(busFacts[u].rebootWatch, s.uptimeSeconds,
                           s.lifetimeBrownoutCount, s.lifetimeWatchdogCount)) {
        SerialPrintf("[unit %d] rebooted (%s); brownouts=%u watchdogs=%u\n",
                     (int)(base + u),
                     unitResetCauseName(unitResetCauseDecode(s.mcusrAtBoot)),
                     (unsigned)s.lifetimeBrownoutCount,
                     (unsigned)s.lifetimeWatchdogCount);
      }
    }
```

(Match the exact loop variable / `base` address expression already used in `heartbeatTick`; read lines 197–260 first.)

- [ ] **Step 3: Build the Master firmware**

Run: `cd firmware/v2/Master && pio run`
Expected: SUCCESS (the PostToolUse hook also re-runs the native suite).

- [ ] **Step 4: Run native suite**

Run: `cd firmware/v2/Master && pio test -e native`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add firmware/v2/Master/UnitHealth.h firmware/v2/Master/DisplayTask.cpp
git commit -m "feat(#368): log unit reboot (reset-cause + counts) in heartbeatTick"
```

### Task A4: HA reset-cause + reboot-count sensors

**Files:**
- Modify: `firmware/v2/Master/MqttHelpers.h` (discovery enum + `buildEntityDiscovery` cases + telemetry payload fn near line 198)
- Modify: `firmware/v2/Master/MqttService.cpp` (publish site near line 602)
- Test: `firmware/v2/Master/test/test_mqtt_helpers/test_mqtt_helpers.cpp`

**Interfaces:**
- Consumes: fleet-worst reset/reboot data. Add a pure `unitFleetRebootTotal(units, width)` helper mirroring `unitFleetVccMin` (sum of `lifetimeBrownoutCount + lifetimeWatchdogCount` across valid units) — the fleet reboot odometer.

- [ ] **Step 1: Write the failing test** for the fleet helper + telemetry key:

```cpp
void test_fleet_reboot_total_sums_valid_units() {
    UnitSnapshot u[2];
    u[0].statusValid = true; u[0].status.lifetimeBrownoutCount = 3; u[0].status.lifetimeWatchdogCount = 1;
    u[1].statusValid = true; u[1].status.lifetimeBrownoutCount = 2; u[1].status.lifetimeWatchdogCount = 0;
    TEST_ASSERT_EQUAL_UINT32(6, unitFleetRebootTotal(u, 2));
}
```

(Use the exact snapshot type `test_mqtt_helpers` already constructs for `unitFleetVccMin`; read that test first.)

- [ ] **Step 2: Run test to verify it fails**

Run: `cd firmware/v2/Master && pio test -e native -f test_mqtt_helpers`
Expected: FAIL — `unitFleetRebootTotal` undefined.

- [ ] **Step 3: Implement** — add `unitFleetRebootTotal` next to `unitFleetVccMin`; add `DISCOVERY_REBOOT_TOTAL` to the discovery enum with a `buildEntityDiscovery(... "_reboot_total", "Unit reboots (fleet)", "telemetry", "{{ value_json.reboots }}", nullptr, nullptr, "diagnostic" ...)` case (state_class total_increasing); append `,"reboots":%u` to the telemetry payload builder; publish it in `MqttService.cpp` alongside the `unitFleetVccMin` call.

- [ ] **Step 4: Run test to verify it passes** + build

Run: `cd firmware/v2/Master && pio test -e native -f test_mqtt_helpers && pio run`
Expected: PASS + build SUCCESS.

- [ ] **Step 5: Commit**

```bash
git add firmware/v2/Master/MqttHelpers.h firmware/v2/Master/MqttService.cpp firmware/v2/Master/test/test_mqtt_helpers/
git commit -m "feat(#368): HA fleet-reboot-total telemetry sensor"
```

---

## SLICE B — GET_EXT_DIAG packet + new signals (one reflash)

### Task B1: Opcode + UnitExtDiag.h pure header (all three trees) + native tests

**Files:**
- Modify: `firmware/v2/shared/SplitFlapProtocol.h` (opcode)
- Create: `firmware/v2/Unit/UnitExtDiag.h`, `firmware/v2/Master/UnitExtDiag.h`, `firmware/v2/FollowerEsp01/UnitExtDiag.h` (identical)
- Modify: `tests/test_copied_headers.py` (add `"UnitExtDiag.h"` to the manifest list)
- Test: `firmware/v2/Unit/test/test_ext_diag/test_ext_diag.cpp` and `firmware/v2/Master/test/test_ext_diag/test_ext_diag.cpp` (identical)

**Interfaces:**
- Produces: `#define EXT_DIAG_REPLY_LEN 11`, `EXT_DIAG_REPLY_CHECKSUM_MASK 0x93`, `EXT_DIAG_STATUS_STALL (1<<0)`; `struct UnitExtDiag`; `extDiagChecksum`, `extDiagEncodeReply`, `extDiagReadbackValid`.

- [ ] **Step 1: Add the opcode** — in `SplitFlapProtocol.h`, in the 0x8X query band after `SFP_CMD_GET_VITALS 0x88`:

```cpp
#define SFP_CMD_GET_EXT_DIAG       0x89  // reply: 11 bytes — step-excess last/max,
                                         //     per-move Vcc sag, hall edges/rev,
                                         //     duty window, status bits + XOR
                                         //     checksum ^ 0x93 (wire format in
                                         //     UnitExtDiag.h, #365). Unversioned;
                                         //     a format change takes a new opcode.
```

- [ ] **Step 2: Write the failing test** — `firmware/v2/Unit/test/test_ext_diag/test_ext_diag.cpp`:

```cpp
#include <unity.h>
#include "UnitExtDiag.h"

void test_roundtrip() {
    UnitExtDiag in;
    in.stepExcessLast = 40; in.stepExcessMax = 512; in.vccSagLastMove = 4321;
    in.hallEdgesLastRev = 1; in.dutyWindow = 77; in.statusBits = EXT_DIAG_STATUS_STALL;
    uint8_t buf[EXT_DIAG_REPLY_LEN];
    extDiagEncodeReply(in, buf);
    UnitExtDiag out;
    TEST_ASSERT_TRUE(extDiagReadbackValid(buf, out));
    TEST_ASSERT_EQUAL_UINT16(40,   out.stepExcessLast);
    TEST_ASSERT_EQUAL_UINT16(512,  out.stepExcessMax);
    TEST_ASSERT_EQUAL_UINT16(4321, out.vccSagLastMove);
    TEST_ASSERT_EQUAL_UINT8(1,     out.hallEdgesLastRev);
    TEST_ASSERT_EQUAL_UINT16(77,   out.dutyWindow);
    TEST_ASSERT_EQUAL_UINT8(EXT_DIAG_STATUS_STALL, out.statusBits);
}
void test_rejects_all_ff() {   // un-flashed unit -> un-ACKed read padding
    uint8_t buf[EXT_DIAG_REPLY_LEN]; for (auto& b : buf) b = 0xFF;
    UnitExtDiag out; TEST_ASSERT_FALSE(extDiagReadbackValid(buf, out));
}
void test_rejects_all_zero() {
    uint8_t buf[EXT_DIAG_REPLY_LEN] = {0};
    UnitExtDiag out; TEST_ASSERT_FALSE(extDiagReadbackValid(buf, out));
}
void test_rejects_bitflip() {
    UnitExtDiag in; uint8_t buf[EXT_DIAG_REPLY_LEN]; extDiagEncodeReply(in, buf);
    buf[3] ^= 0x20;
    UnitExtDiag out; TEST_ASSERT_FALSE(extDiagReadbackValid(buf, out));
}
int main() { UNITY_BEGIN();
  RUN_TEST(test_roundtrip); RUN_TEST(test_rejects_all_ff);
  RUN_TEST(test_rejects_all_zero); RUN_TEST(test_rejects_bitflip);
  return UNITY_END(); }
```

Note: `test_rejects_all_zero` requires the mask to make an all-zero payload's checksum ≠ 0 — 0x93 ≠ 0 guarantees it. Copy this file identically to `firmware/v2/Master/test/test_ext_diag/`.

- [ ] **Step 3: Run test to verify it fails**

Run: `cd firmware/v2/Unit && pio test -e native -f test_ext_diag`
Expected: FAIL — `UnitExtDiag.h` not found.

- [ ] **Step 4: Create `UnitExtDiag.h`** (identical in all three trees):

```cpp
#pragma once
// Pure logic for the SFP_CMD_GET_EXT_DIAG reply (#365) — the unit's
// new-measurement diagnostics that GET_STATUS/GET_VITALS don't already carry.
// Natively tested by test_ext_diag. AVR glue that FEEDS it lives in the .ino
// files (bench tier).
//
// SHARED header: copied verbatim into Unit, Master and FollowerEsp01. Fix bugs
// in ALL three trees (copy policy; tests/test_copied_headers.py gates it).
//
// Wire format (SFP_CMD_GET_EXT_DIAG reply, 11 bytes):
//   off  field            type    notes
//   0..1 stepExcessLast   u16 LE  last home: actual - geometry-expected steps
//   2..3 stepExcessMax    u16 LE  worst-seen excess since boot (drag alarm)
//   4..5 vccSagLastMove   u16 LE  min Vcc during last move (mV)
//   6    hallEdgesLastRev u8      entering-edges in last completed rev (1=OK)
//   7..8 dutyWindow       u16 LE  moves in a rolling ~60s window
//   9    statusBits       u8      bit0 last-move stall/jam; bits1-7 reserved
//   10   checksum         u8      XOR of 0..9 ^ EXT_DIAG_REPLY_CHECKSUM_MASK
//
// Backward compat (#231/#106 pattern): a pre-ext-diag unit answers the unknown
// opcode with its 1-byte status reply + bus padding; the masked checksum
// rejects all-0xFF, all-0x00 and repeated-status garbage, so the master
// degrades to diagExt=false and emits no ext fields — never a phantom reading.

#include <stdint.h>

#define EXT_DIAG_REPLY_LEN            11
#define EXT_DIAG_REPLY_CHECKSUM_MASK  0x93
#define EXT_DIAG_STATUS_STALL         (1 << 0)

struct UnitExtDiag {
  uint16_t stepExcessLast = 0;
  uint16_t stepExcessMax = 0;
  uint16_t vccSagLastMove = 0;
  uint8_t  hallEdgesLastRev = 0;
  uint16_t dutyWindow = 0;
  uint8_t  statusBits = 0;
};

inline uint8_t extDiagChecksum(const uint8_t buf[EXT_DIAG_REPLY_LEN]) {
  uint8_t x = 0;
  for (uint8_t i = 0; i < EXT_DIAG_REPLY_LEN - 1; i++) x ^= buf[i];
  return (uint8_t)(x ^ EXT_DIAG_REPLY_CHECKSUM_MASK);
}

inline void extDiagEncodeReply(const UnitExtDiag& d, uint8_t buf[EXT_DIAG_REPLY_LEN]) {
  buf[0] = (uint8_t)(d.stepExcessLast & 0xFF);
  buf[1] = (uint8_t)((d.stepExcessLast >> 8) & 0xFF);
  buf[2] = (uint8_t)(d.stepExcessMax & 0xFF);
  buf[3] = (uint8_t)((d.stepExcessMax >> 8) & 0xFF);
  buf[4] = (uint8_t)(d.vccSagLastMove & 0xFF);
  buf[5] = (uint8_t)((d.vccSagLastMove >> 8) & 0xFF);
  buf[6] = d.hallEdgesLastRev;
  buf[7] = (uint8_t)(d.dutyWindow & 0xFF);
  buf[8] = (uint8_t)((d.dutyWindow >> 8) & 0xFF);
  buf[9] = d.statusBits;
  buf[10] = extDiagChecksum(buf);
}

inline bool extDiagReadbackValid(const uint8_t buf[EXT_DIAG_REPLY_LEN], UnitExtDiag& out) {
  if (buf[EXT_DIAG_REPLY_LEN - 1] != extDiagChecksum(buf)) return false;
  out.stepExcessLast = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
  out.stepExcessMax  = (uint16_t)buf[2] | ((uint16_t)buf[3] << 8);
  out.vccSagLastMove = (uint16_t)buf[4] | ((uint16_t)buf[5] << 8);
  out.hallEdgesLastRev = buf[6];
  out.dutyWindow = (uint16_t)buf[7] | ((uint16_t)buf[8] << 8);
  out.statusBits = buf[9];
  return true;
}
```

- [ ] **Step 5: Add to the copied-header manifest** — in `tests/test_copied_headers.py`, add `"UnitExtDiag.h"` to the same list that holds `"UnitVitals.h"`.

- [ ] **Step 6: Run tests to verify they pass**

Run: `cd firmware/v2/Unit && pio test -e native -f test_ext_diag && cd ../Master && pio test -e native -f test_ext_diag && cd /home/lucas/split-flap && python -m pytest tests/test_copied_headers.py`
Expected: PASS (all three).

- [ ] **Step 7: Commit**

```bash
git add firmware/v2/shared/SplitFlapProtocol.h firmware/v2/*/UnitExtDiag.h \
        firmware/v2/Unit/test/test_ext_diag firmware/v2/Master/test/test_ext_diag \
        tests/test_copied_headers.py
git commit -m "feat(#365): GET_EXT_DIAG opcode 0x89 + UnitExtDiag.h wire format (3 trees)"
```

### Task B2: Unit-side glue — measure + populate the ext-diag reply

**Files:**
- Modify: `firmware/v2/Unit/Unit.ino` (globals: `#include "UnitExtDiag.h"`, reply buf, measurement state)
- Modify: `firmware/v2/Unit/UnitI2CProtocol.ino` (dispatch case + requestEvent case)
- Modify: `firmware/v2/Unit/UnitMotion.ino` (excess in `calibrate`, sag/stall in `rotateToLetter`, hall edges/rev, duty window; encode into buf)

**Interfaces:**
- Consumes: `extDiagEncodeReply` (B1); existing `calibrate()` step count, `vitalsVccNow`, `rotateToLetter` step/speed, `driftObserveEdge`, `stepAccumulator`.
- Produces: `volatile uint8_t extDiagReplyBuf[EXT_DIAG_REPLY_LEN]`; `pendingExtDiagResponse`. This is AVR/bench tier — verified on hardware, not natively.

- [ ] **Step 1: Declare globals in `Unit.ino`** (near `vitalsReplyBuf`, line ~210):

```cpp
volatile bool     pendingExtDiagResponse = false;
volatile uint8_t  extDiagReplyBuf[EXT_DIAG_REPLY_LEN] = {0};
uint16_t          extStepExcessLast = 0;
uint16_t          extStepExcessMax  = 0;
uint16_t          extVccSagLastMove = 0xFFFF;  // sentinel high; reset per move
uint8_t           extHallEdgesLastRev = 0;
uint16_t          extHallEdgesThisRev = 0;
uint16_t          extDutyWindow = 0;
uint8_t           extStatusBits = 0;
```

Add `#include "UnitExtDiag.h"` near the other Unit*.h includes.

- [ ] **Step 2: Dispatch case in `receiveLetter` (`UnitI2CProtocol.ino`, in the opcode switch):**

```cpp
      case SFP_CMD_GET_EXT_DIAG:
        pendingExtDiagResponse = true;
        break;
```

- [ ] **Step 3: Reply case in `requestEvent` (`UnitI2CProtocol.ino`, mirroring the vitals block ~187):**

```cpp
  if (pendingExtDiagResponse) {
    // #365 — stream the loop-side-built buffer verbatim. Un-reflashed masters
    // never send GET_EXT_DIAG; the masked checksum handles the reverse.
    Wire.write((const uint8_t*)extDiagReplyBuf, EXT_DIAG_REPLY_LEN);
    pendingExtDiagResponse = false;
    return;
  }
```

- [ ] **Step 4: Measurement glue in `UnitMotion.ino`:**
  - In `calibrate()`, after the hall edge is found: compute `expected` steps from the believed `drumPosition` to the edge (rotation-direction distance), `excess = actual >= expected ? actual - expected : 0`; set `extStepExcessLast = excess`; `if (excess > extStepExcessMax) extStepExcessMax = excess;`.
  - In `rotateToLetter()` entry: `extVccSagLastMove = 0xFFFF;` and bump the duty window (`if (extDutyWindow < 0xFFFF) extDutyWindow++;` — decayed on a coarse timer in `loop()`); on each mid-move Vcc sample (same site as `vitalsVccMin` at line 347): `if (vitalsVccNow && vitalsVccNow < extVccSagLastMove) extVccSagLastMove = vitalsVccNow;`.
  - In `rotateToLetter()` exit: compute `expectedMs` from commanded steps × step period; `if (actualMs > expectedMs + (expectedMs >> 2)) extStatusBits |= EXT_DIAG_STATUS_STALL; else extStatusBits &= ~EXT_DIAG_STATUS_STALL;`.
  - Hall edges/rev: in the odometer path where `stepAccumulator` wraps a revolution, latch `extHallEdgesLastRev = extHallEdgesThisRev; extHallEdgesThisRev = 0;`; increment `extHallEdgesThisRev` wherever `driftObserveEdge` sees an entering edge.
  - Add a `refreshExtDiagReply()` (mirror `refreshVitalsReply` at ~355) that fills a `UnitExtDiag`, calls `extDiagEncodeReply`, and copies into `extDiagReplyBuf`; call it on the same loop cadence the vitals refresh uses.

- [ ] **Step 5: Build the Unit firmware (both envs)**

Run: `cd firmware/v2/Unit && pio run -e unit`
Expected: SUCCESS. (Native suite has no new unit-glue tests — glue is bench tier.)

- [ ] **Step 6: Commit**

```bash
git add firmware/v2/Unit/Unit.ino firmware/v2/Unit/UnitI2CProtocol.ino firmware/v2/Unit/UnitMotion.ino
git commit -m "feat(#365): unit-side GET_EXT_DIAG glue — excess/sag/hall/duty/stall"
```

### Task B3: Master-side read + fold into UnitFacts

**Files:**
- Modify: `firmware/v2/Master/UnitHealth.h:61` (`UnitFacts`: `UnitExtDiag extDiag{}; bool extDiagValid = false;` + `#include "UnitExtDiag.h"`)
- Modify: `firmware/v2/Master/UnitBus.cpp` (`readUnitExtDiag` + `refreshUnitExtDiag`, called from `unitBusPollHealthOne` and the scan)

**Interfaces:**
- Consumes: `extDiagReadbackValid` (B1), `queryUnit` (existing).
- Produces: `UnitFacts.extDiag`, `UnitFacts.extDiagValid`.

- [ ] **Step 1: Add fields to `UnitFacts`** (in `UnitHealth.h`, next to `vitals`/`vitalsValid`):

```cpp
  UnitExtDiag extDiag{};        // #365 GET_EXT_DIAG new-measurement signals
  bool        extDiagValid = false;
```

Add `#include "UnitExtDiag.h"` at the top of `UnitHealth.h`.

- [ ] **Step 2: Add read + refresh in `UnitBus.cpp`** (mirror `readUnitVitals`/`refreshUnitVitals` ~219–234):

```cpp
static bool readUnitExtDiag(int i2cAddress, UnitExtDiag& out) {
  uint8_t buf[EXT_DIAG_REPLY_LEN];
  if (!queryUnit(i2cAddress, (uint8_t)SFP_CMD_GET_EXT_DIAG, buf, EXT_DIAG_REPLY_LEN)) return false;
  return extDiagReadbackValid(buf, out);
}
static void refreshUnitExtDiag(UnitFacts& fact, int i2cAddress) {
  fact.extDiagValid = false;
  UnitExtDiag d;
  if (!readUnitExtDiag(i2cAddress, d)) return;
  fact.extDiag = d;
  fact.extDiagValid = true;
}
```

- [ ] **Step 3: Call it on the round-robin health poll** — in `unitBusPollHealthOne` after the `refreshUnitVitals` line (~527), add `refreshUnitExtDiag(facts[i], toI2cAddress(i));`. Also add `refreshUnitExtDiag(facts[unitIndex], i2cAddress);` in the scan block after the `refreshUnitVitals` call (~480). This inherits the existing per-tick round-robin cadence (one unit per heartbeat tick) — matching the spec's flat-load intent; ext-diag sub-reads are NOT charged to #367 error attribution (they fail routinely on pre-opcode firmware).

- [ ] **Step 4: Build + native**

Run: `cd firmware/v2/Master && pio run && pio test -e native`
Expected: SUCCESS + PASS.

- [ ] **Step 5: Commit**

```bash
git add firmware/v2/Master/UnitHealth.h firmware/v2/Master/UnitBus.cpp
git commit -m "feat(#365): master reads GET_EXT_DIAG round-robin into UnitFacts"
```

### Task B4: /units/health JSON keys + buffer-cap headroom

**Files:**
- Modify: `firmware/v2/Master/UnitHealth.h` (JSON append ~244–256, behind `extDiagValid`)
- Test: `firmware/v2/Master/test/test_unit_health/` (worst-case buffer capacity test)

**Interfaces:**
- Consumes: `UnitFacts.extDiag`, `UnitFacts.extDiagValid`.
- Produces: JSON keys `se`, `sx`, `sag`, `he`, `dw`, `sb` (only when `extDiagValid`).

- [ ] **Step 1: Extend the worst-case JSON capacity test** — in `test_unit_health`, set every unit `extDiagValid=true` with max-width values (all `0xFFFF`/`0xFF`) and assert the built string still fits `UNIT_HEALTH_JSON_CAP`. Run it first; if it overflows, that's the RED.

Run: `cd firmware/v2/Master && pio test -e native -f test_unit_health`
Expected: FAIL if new keys not yet emitted / cap too small.

- [ ] **Step 2: Emit the keys** — in the `if (u.statusValid)` JSON block, after the existing status keys, add a nested guarded append:

```cpp
    if (u.extDiagValid) {
      const UnitExtDiag& e = u.extDiag;
      UNIT_HEALTH_APPEND(",\"se\":%u,\"sx\":%u,\"sag\":%u,\"he\":%u,\"dw\":%u,\"sb\":%u",
                         (unsigned)e.stepExcessLast, (unsigned)e.stepExcessMax,
                         (unsigned)e.vccSagLastMove, (unsigned)e.hallEdgesLastRev,
                         (unsigned)e.dutyWindow, (unsigned)e.statusBits);
    }
```

- [ ] **Step 3: Bump `UNIT_HEALTH_JSON_CAP` if the test overflows** — the six keys add ≤ ~48 bytes/unit; raise the cap (currently 6144 after #367) to `7168` if needed. Keep the ESP-01 follower buffer local (do not blow its 5120 — the follower emits a trimmed set; see B7).

- [ ] **Step 4: Run to verify pass** + build

Run: `cd firmware/v2/Master && pio test -e native -f test_unit_health && pio run`
Expected: PASS + build SUCCESS.

- [ ] **Step 5: Commit**

```bash
git add firmware/v2/Master/UnitHealth.h firmware/v2/Master/test/test_unit_health/
git commit -m "feat(#365): surface GET_EXT_DIAG fields in /units/health JSON"
```

### Task B5: Event-log edges — jam / drag / hall-anomaly

**Files:**
- Modify: `firmware/v2/Master/UnitEventLog.h` (new event bits + condition builder input)
- Modify: `firmware/v2/Master/DisplayTask.cpp` (feed ext-diag into the #322 condition mask + label strings)
- Test: `firmware/v2/Master/test/test_unit_event_log/`

**Interfaces:**
- Consumes: `UnitExtDiag` fields, existing `unitEventMask`/transition machinery.
- Produces: `#define UNIT_EVT_JAM (1<<5)`, `UNIT_EVT_DRAG (1<<6)`, `UNIT_EVT_HALL_ANOMALY (1<<7)`; a threshold constant `EXT_DIAG_DRAG_EXCESS_STEPS`.

- [ ] **Step 1: Write the failing test** — a unit with `statusBits & STALL` produces `UNIT_EVT_JAM` in the derived mask; `stepExcessMax > threshold` → `UNIT_EVT_DRAG`; `hallEdgesLastRev != 1` (and read valid) → `UNIT_EVT_HALL_ANOMALY`; all onset-only (not in `UNIT_EVT_RECOVERABLE`). Extend the existing mask-builder test with an ext-diag input struct.

Run: `cd firmware/v2/Master && pio test -e native -f test_unit_event_log`
Expected: FAIL.

- [ ] **Step 2: Add bits + threshold to `UnitEventLog.h`:**

```cpp
#define UNIT_EVT_JAM          (1 << 5)  // last move stalled (ext-diag statusBits)
#define UNIT_EVT_DRAG         (1 << 6)  // steps-to-home excess over threshold
#define UNIT_EVT_HALL_ANOMALY (1 << 7)  // hall edges/rev != 1 (degrading sensor)
// Excess steps over the geometry-expected home distance that flags mechanical
// drag. Well above normal slop, below a jam. TODO(#370): tune on the bench.
#define EXT_DIAG_DRAG_EXCESS_STEPS 200
```

Fold these into the same mask the builder returns (jam/drag/hall-anomaly are onset-only, so NOT added to `UNIT_EVT_RECOVERABLE`). Extend the mask-builder signature to accept the ext-diag inputs (valid flag + statusBits + stepExcessMax + hallEdgesLastRev); when `!extDiagValid`, contribute none of the three bits (a silent/pre-flash unit never fabricates a jam).

- [ ] **Step 3: Wire labels + inputs in `DisplayTask.cpp`** — add the ext-diag args to the builder call in `heartbeatTick`, and label strings: `"jam (stalled move)"`, `"steps-to-home drag"`, `"hall edges/rev anomaly"` in the transition-emit switch.

- [ ] **Step 4: Run to verify pass** + build + full native

Run: `cd firmware/v2/Master && pio test -e native -f test_unit_event_log && pio run && pio test -e native`
Expected: PASS + SUCCESS.

- [ ] **Step 5: Commit**

```bash
git add firmware/v2/Master/UnitEventLog.h firmware/v2/Master/DisplayTask.cpp firmware/v2/Master/test/test_unit_event_log/
git commit -m "feat(#365): log jam/drag/hall-anomaly transitions from GET_EXT_DIAG"
```

### Task B6: HA jam binary_sensor + steps-excess sensor

**Files:**
- Modify: `firmware/v2/Master/MqttHelpers.h` (2 discovery enum entries + cases + telemetry keys)
- Modify: `firmware/v2/Master/MqttService.cpp` (publish)
- Test: `firmware/v2/Master/test/test_mqtt_helpers/`

**Interfaces:**
- Consumes: fleet-worst ext-diag. Add pure `unitFleetAnyJam(units, width)` (bool: any valid unit `statusBits & STALL`) and `unitFleetMaxExcess(units, width)` (max `stepExcessMax`).

- [ ] **Step 1: Write failing tests** for both fleet helpers (mirror `unitFleetVccMin` test shape).

Run: `cd firmware/v2/Master && pio test -e native -f test_mqtt_helpers`
Expected: FAIL.

- [ ] **Step 2: Implement** — helpers next to `unitFleetVccMin`; `DISCOVERY_UNIT_JAM` (`binary_sensor`, device_class `problem`, `{{ value_json.jam }}`) and `DISCOVERY_STEPS_EXCESS` (`sensor`, `{{ value_json.stepsExcess }}`, unit `steps`, diagnostic); append `,"jam":%s,"stepsExcess":%u` to the unit-telemetry payload; publish in `MqttService.cpp`.

- [ ] **Step 3: Run to verify pass** + build

Run: `cd firmware/v2/Master && pio test -e native -f test_mqtt_helpers && pio run`
Expected: PASS + SUCCESS.

- [ ] **Step 4: Commit**

```bash
git add firmware/v2/Master/MqttHelpers.h firmware/v2/Master/MqttService.cpp firmware/v2/Master/test/test_mqtt_helpers/
git commit -m "feat(#365): HA jam binary_sensor + fleet steps-excess sensor"
```

### Task B7: FollowerEsp01 parity

**Files:**
- Modify: `firmware/v2/FollowerEsp01/` — ext-diag read into its `/units/health`, api-index gate, buffer.
- Test: `firmware/v2/FollowerEsp01/test/` + `tests/test_api_index.py`

**Interfaces:**
- Consumes: `UnitExtDiag.h` (copied in B1), the follower's existing unit-health read path.

- [ ] **Step 1: Read** the follower's unit-health builder + its I2C read path (`git grep GET_VITALS firmware/v2/FollowerEsp01`); it mirrors the Master pattern at smaller scale.

- [ ] **Step 2: Add** the `GET_EXT_DIAG` read + the same six JSON keys (guarded by an ext-valid flag), keeping the follower's local 5120 buffer (trim: the follower row is ≤ a handful of units, so headroom holds; add a capacity test if near).

- [ ] **Step 3: Update `tests/test_api_index.py`** if the follower's `/units/health` shape is asserted there; keep `/cluster/*` notServed array unchanged (ext-diag adds no route).

- [ ] **Step 4: Build + native + python**

Run: `cd firmware/v2/FollowerEsp01 && pio run && pio test -e native && cd /home/lucas/split-flap && python -m pytest tests/test_api_index.py tests/test_copied_headers.py`
Expected: SUCCESS + PASS.

- [ ] **Step 5: Commit**

```bash
git add firmware/v2/FollowerEsp01 tests/test_api_index.py
git commit -m "feat(#365): FollowerEsp01 surfaces GET_EXT_DIAG in /units/health"
```

### Task C1: Stage the unit bundle (separate artifact commit)

**Files:**
- Modify: `firmware/v2/Master/data/unit-firmware.hex` + `.rev`, `firmware/v2/FollowerEsp01/data/unit-firmware.hex` + `.rev` (generated).

- [ ] **Step 1: Rebuild Unit clean**

Run: `cd firmware/v2/Unit && pio run -e unit`
Expected: SUCCESS (fresh hex).

- [ ] **Step 2: Stage into both masters' data dirs**

Run: `cd /home/lucas/split-flap && python flashing/flasher/make_manifest.py stage`
Expected: writes `unit-firmware.hex` + `.rev` into Master AND FollowerEsp01 `data/`.

- [ ] **Step 3: Rebuild Master + FollowerEsp01 (drift gate)**

Run: `cd firmware/v2/Master && pio run && cd ../FollowerEsp01 && pio run && cd /home/lucas/split-flap && python -m pytest tests/`
Expected: SUCCESS + all python gates PASS (bundle drift green).

- [ ] **Step 4: Separate artifact commit**

```bash
git add firmware/v2/Master/data firmware/v2/FollowerEsp01/data
git commit -m "chore(#365): stage unit-ext-diag firmware bundle"
```

---

## Post-plan: review, bench, ship (not code tasks)

- **cpp-reviewer** over the full branch diff (I2C-wire touched → ALWAYS review); address CRITICAL/HIGH.
- **Bench (E2E tier):** OTA leader-first → `/reflash-units` per master. Verify: reset-cause line on a real power-cycle; jam bit by stalling a flap; excess trend healthy vs dragging; hall edges = 1 healthy. Pre-flight: confirm wall units are DIP-addressed (EEPROM==DIP) so twiboot is reachable.
- **PR** batching #368–#374; close each with a keyword (auto-close has failed before — verify).
- **Release:** fold into the day's CalVer cut if worth announcing (BREAKING? no — additive opcode + JSON keys).

---

## Self-Review

**Spec coverage:**
- #368 reset-cause/reboot/uptime → A1/A2/A3 (log) + A4 (HA). Data already on wire (GET_STATUS). ✓
- #369 bad-command counter → already on wire + JSON (`bc`); no new field (spec: counter-only). Surfaced today via existing `bc`. ✓ (No dedicated task — it is already end-to-end; noted in spec §"Why #369 ships as the counter only".)
- #370 steps-to-home excess → B1 (fields) + B2 (calibrate glue) + B4 (JSON) + B5 (drag log) + B6 (HA excess). ✓
- #371 per-move Vcc sag → B1/B2/B4 (`sag` key). ✓
- #372 hall edges/rev → B1/B2/B4 + B5 (hall-anomaly log). ✓
- #373 duty window → B1/B2/B4 (`dw` key). ✓
- #374 stall bit → B1 (`EXT_DIAG_STATUS_STALL`) + B2 (glue) + B5 (jam log) + B6 (jam HA). ✓
- Round-robin cadence → B3 (folded into `unitBusPollHealthOne`, already per-tick). ✓
- Copy policy / drift gates → B1 (manifest), B7 (api-index). ✓
- Bundle flow → C1. ✓

**Placeholder scan:** AVR-glue steps (B2, B7) describe the change with concrete sites + code fragments rather than full RED-GREEN, because that glue is bench tier (ArduinoFake can't drive the ADC/hall/TWI ISR). Pure headers + master decode + HA all carry full test code. No "TBD"/"handle edge cases" left.

**Type consistency:** `UnitExtDiag`, `extDiagReadbackValid`, `EXT_DIAG_REPLY_LEN`, `extDiagValid`, `extDiag` used identically across B1/B3/B4/B5/B6. `EXT_DIAG_STATUS_STALL` defined B1, used B2/B5/B6. Event bits `UNIT_EVT_JAM/DRAG/HALL_ANOMALY` defined + consumed only in B5. `UnitRebootWatch`/`unitRebootDetect` defined A2, consumed A3. ✓
