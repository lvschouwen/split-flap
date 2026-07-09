# v2 Master task & memory architecture (#58 skeleton slice)

**Date:** 2026-07-09
**Scope:** `firmware/v2/Master/` — the FreeRTOS task skeleton, IPC contract, and
memory policy that all subsequent #58 slices (WiFi, I2C port, clock, MQTT) land
into. Contract-only on the display side: the display worker is a logging stub;
the real I2C port is a later slice.

## Motivation

The ESP32-S3 is dual-core with ~400 KB usable internal SRAM (vs the ESP-01's
~37 KB free) and optionally 8 MB PSRAM. v1's deepest architectural pain was
single-core contention: blocking I2C unit transactions stall the async web
server, which forced a family of improvised workarounds (MQTT callbacks
copy+flag only, mDNS discovery deferred to `loop()`, `showMessage()`
snapshotting `displayWidth` mid-call). v2 makes that discipline structural:
one core owns the display, the other owns the network, and everything crosses
between them through explicit queues and snapshots. Deciding this before the
WiFi slice means every later slice has a pre-built home instead of accreting
into `loop()`.

## Decisions taken (2026-07-09, with user)

1. **Slice order:** task skeleton lands before the WiFi join/portal slice.
2. **Display scope:** contract only — command queue + snapshot seam + stub
   worker that logs commands. I2C port is a later dedicated slice.
3. **Architecture:** full task decomposition (user choice; actor-style
   single-owner was the alternative) — all domain tasks exist from day one,
   stub-bodied until their slice arrives.

## 1. Task & core layout

**Core 1 — display domain.** Timing-sensitive work; never blocks on network.

| Task | Priority | Stack | Role |
|------|----------|-------|------|
| `displayTask` | 3 | 4 KB | Exclusive owner of I2C/`Wire`. Blocks on the command queue; executes probe / show-text / calibration / unit-reflash commands. *This slice: stub worker logging each command.* |
| `clockTask` | 1 | 2 KB | 1 Hz ticker; in clock mode composes the time string and enqueues a show-text command. *This slice: logs ticks ("time unknown"); real time arrives with the NTP slice.* |

**Core 0 — network domain.** Shares the core with WiFi/LWIP/BT, which the
Arduino framework already pins there.

| Task | Priority | Stack | Role |
|------|----------|-------|------|
| `netTask` | 1 | 4 KB | Housekeeping: absorbs `webEndpointsLoop()` (deferred settings commits, pending settings posts); later WiFi supervision and mDNS discovery runs. |
| `mqttTask` | 1 | 4 KB | Owns the MQTT client lifecycle. LWIP callbacks only copy into its inbox queue (v1's copy+flag rule, now structural). *This slice: idle stub.* |

- AsyncTCP's worker task is pinned to core 0 via
  `-DCONFIG_ASYNC_TCP_RUNNING_CORE=0`, so all network execution lives on
  core 0.
- Arduino's `loopTask` remains only as the observability heartbeat: every 5 s
  it prints free heap plus each task's `uxTaskGetStackHighWaterMark` —
  empirical validation of the stack sizing table on real hardware. `setup()`
  stays the composition root.
- Priorities: `displayTask` at 3 outranks everything application-level so flap
  motion timing wins on its core; all other app tasks run at 1 (tskIDLE + 1).

## 2. IPC contract

- **`DisplayCommand`** — small POD struct: opcode (`ShowText`, `Probe`,
  `Calibrate`, `SetOffset`, …), fixed-size text buffer (`UNITS_AMOUNT` + NUL),
  and all parameters (speed, alignment) **baked in by the sender**. Rule:
  commands carry everything they need — tasks never reach across cores into
  shared settings.
- **Command queue** — statically allocated FreeRTOS queue, 16 deep, into
  `displayTask`. Enqueue is non-blocking (`timeout 0`); a full queue is
  reported to the caller (HTTP 503 on the web path), never waited on from a
  network context.
- **`DisplaySnapshot`** — display width, per-unit status/rev, busy flag,
  current text. Written only by `displayTask` behind a short-held mutex;
  consumers use a copy-out getter and build JSON from their copy, never from
  live state.
- **MQTT inbox queue** — statically allocated; LWIP-context callbacks copy
  topic-index + payload in and return. Consumed only by `mqttTask`.
- Command construction/validation and snapshot copy semantics live in pure
  headers (`DisplayCommand.h`, `DisplayIpc.h`) exercised by native unit tests,
  same pattern as the existing ported suites. FreeRTOS calls stay out of the
  pure headers so the native env needs no RTOS fake.
- **Exemplar path (this slice):** the web message-POST handler builds and
  enqueues a `ShowText` command instead of answering 501 — proving the
  network→display seam end to end. All other unported endpoints stay 501.

## 3. Memory policy

1. **Static allocation for RTOS objects.** All task stacks and queues are
   statically allocated (`xTaskCreateStaticPinnedToCore`,
   `xQueueCreateStatic`) in internal SRAM — deterministic footprint, visible
   in the map file, zero heap fragmentation from task churn.
2. **`largeAlloc()` helper** for big elastic buffers:
   `heap_caps_malloc(MALLOC_CAP_SPIRAM)` with internal-RAM fallback, so the
   same binary is correct on any devkit variant with or without PSRAM. First
   consumer: the WebLog ring grows from v1's 2 KB to 32 KB.
3. **PSRAM build flags deferred to hardware.** `BOARD_HAS_PSRAM` +
   `board_build.arduino.memory_type` get locked when the physical devkit's
   module marking is read (expected 2026-07-10); a marked TODO in
   `platformio.ini` holds the spot. Nothing in this slice depends on PSRAM.
4. **Internal-SRAM-only zones (stated as a rule in code):** task stacks, ISR
   data, DMA buffers, and `Wire` buffers — hardware/RTOS requirements.

## 4. What changes where

- `firmware/v2/Master/Tasks.h` / `Tasks.cpp` (new) — task creation, static
  stacks/queues, core pinning, heartbeat high-water reporting.
- `DisplayCommand.h`, `DisplayIpc.h` (new, pure) — command struct +
  build/validate, snapshot copy logic.
- `main.cpp` — `setup()` becomes composition root (settings → identity → IPC →
  tasks → web routes); `loop()` shrinks to the heartbeat.
- `WebEndpoints.cpp` — message-POST handler enqueues `ShowText`;
  `webEndpointsLoop()` body moves under `netTask`.
- `WebLog.*` — ring buffer allocated via `largeAlloc()` at 32 KB.
- `platformio.ini` — AsyncTCP core-pin flag; PSRAM TODO comment.

## 5. Testing & verification

- **Native (TDD):** new suites `test_display_command` (build/validate,
  clamping, text truncation) and `test_display_ipc` (snapshot copy semantics)
  alongside the existing 154 tests.
- **On-device (devkit, 2026-07-10):** boot log prints each task with its core
  ID; heartbeat shows per-task stack high-water marks; once the WiFi slice
  lands, a message POST visibly arrives in the display task's log.
- **CI:** unchanged — `pio run` (master env) + `pio test -e native` +
  `python -m pytest tests/`.

## Out of scope (later slices)

WiFi join/portal (next slice after this), real I2C unit communication, NTP
clock, MQTT client behavior, PSRAM flag lock (hardware-gated), any v1-side
changes.
