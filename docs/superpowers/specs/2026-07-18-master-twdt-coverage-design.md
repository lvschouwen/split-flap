# Master TWDT app-task coverage (#314)

**Status:** design approved 2026-07-18
**Scope:** `firmware/v2/Master` only. Ships as a v2.0.1 reliability patch (post-v2.0.0), or folded into a "harden the wall" v2.1 arc.
**Risk tier:** boot/reliability change → risk-tiered review always applies; bench E2E is the gate.

## Motivation

For an unattended wall-mounted display, a genuinely hung application task should auto-recover via reboot. The ESP-IDF Task Watchdog (TWDT) is already enabled but under-scoped: it watches only the **core-0 idle task** at 5 s with panic-reboot. That catches a core-0 task that *busy-spins* (starves idle), but **not** a task that blocks forever on a queue/mutex (it yields → idle keeps feeding the dog → hang undetected). The entire **display domain on core 1 is unwatched**.

## Current state (`firmware/v2/Master`, generated `sdkconfig`)

- `CONFIG_ESP_TASK_WDT_EN=y`, `CONFIG_ESP_TASK_WDT_INIT=y` — TWDT armed by the framework at boot.
- `CONFIG_ESP_TASK_WDT_PANIC=y` — a timeout **reboots** (panic), not just warns.
- `CONFIG_ESP_TASK_WDT_TIMEOUT_S=5`.
- `CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0=y`; **CPU1 not set**.

Note: `Master/sdkconfig.defaults` is a **generated, gitignored** artifact — NOT the source of truth. Persistent sdkconfig overrides live in `platformio.ini`'s `custom_sdkconfig` (the same mechanism carrying `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`).

## Design

### 1. Timeout → 30 s (config)

Add to `custom_sdkconfig` in `firmware/v2/Master/platformio.ini`:

```
CONFIG_ESP_TASK_WDT_TIMEOUT_S=30
```

Overrides the framework's 5 s. Real hangs are still caught fast; 30 s is well clear of any legitimate blocking op. Keep `PANIC=y` and the existing CPU0 idle check. Do **not** enable `CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1` — explicit subscription of the two core-1 tasks already catches their busy-spins, so the idle-CPU1 check is redundant false-trip surface. Because `ESP_TASK_WDT_INIT=y`, the dog is armed at 30 s from boot; no runtime `esp_task_wdt_reconfigure()` is needed.

### 2. Subscribe all 5 application tasks

The five FreeRTOS tasks (created in `Tasks.cpp` via `xTaskCreateStaticPinnedToCore`):

| task | core | cadence |
|------|------|---------|
| `displayTask` | 1 | event/queue-driven |
| `clockTask` | 1 | 1 Hz tick |
| `netTask` | 0 | periodic |
| `mqttTask` | 0 | periodic |
| `clusterTask` | 0 | periodic tick |

Each `*TaskMain` calls `esp_task_wdt_add(NULL)` **once** at task start (after local init, before the loop), then `esp_task_wdt_reset()` at the **top of its loop**. clockTask's 1 Hz cadence and its bounded "stand down during a cluster commitAt render" wait (~seconds) are both far under 30 s, so it feeds safely every iteration.

### 3. Feed-inside pattern — new header `TaskWatchdog.h`

**Decision (revised after Codex cross-review):** feed the watchdog *inside* long ops rather than unsubscribing around them. The original unsubscribe (`esp_task_wdt_delete`/re-add) approach was rejected because it **blinds the dog to exactly the hang class #314 targets**: the long ops are I2C/network-bound, and this project has a documented history of the IDF I2C driver *wedging* (the 5.5 stale-read crash-loop). If a "self-bounded" transaction's own timeout fails, an unsubscribed task hangs forever undetected. Feeding inside keeps detection live — a wedge between two feed points still trips within ~30 s.

`TaskWatchdog.h` holds two thin target-only helpers:
- `wdtSubscribeSelf()` — `esp_task_wdt_add(NULL)`; treat `ESP_OK` as success, otherwise log `esp_err_to_name(err)`; never fatal. If idempotence matters, gate on `esp_task_wdt_status(NULL)` first.
- `wdtFeed()` — thin wrapper on `esp_task_wdt_reset()` for the current task, called at loop tops and at progress points inside long loops.

No pure logic → no native test (consistent with the project rule: host tests cover pure logic only, hardware glue is bench-verified).

### 4. Feed points

Call `wdtFeed()` at the top of each of the 5 task loops, plus at these progress points inside long ops so no single inter-feed span approaches 30 s:

- **`runReflashJob`** (displayTask) — feed at each per-batch (batch size 4) / per-unit boundary. A single unit page-write is I2C-bounded (ms); a driver wedge between boundaries still trips the dog.
- **self-test poll loops** and **boot-home step sequence** (displayTask) — feed per poll / per homing step.
- **cluster fan-out** (clusterTask) — feed after each member's HTTP op in `clusterLeaderTick()`. Worst case is up to 8 members × 1.5 s plus a ~10 s rollout/follower-push finalize, which can approach 30 s in one tick, so per-member feeding is **required**, not conditional; it also preserves hang detection for the cluster transport path.
- **`MDNS.queryService`** (netTask) — **no special handling**: it takes its own timeout arg (< 30 s), so the netTask loop-top `wdtFeed()` already covers it.
- **shared `UnitBus.cpp` blocking waits called from displayTask** — feed inside their poll loops:
  - `waitForDisplayToStop()` polls up to `SHOW_STUCK_TIMEOUT_MS` (currently == the 30 s TWDT window) and runs **twice** per `unitBusShowFrame()` (entry + exit), so a jammed flap can block ~60 s. Without a feed this reboot-loops on any stuck flap (a common mechanical failure the code otherwise survives). This constant must stay ≤ the TWDT timeout OR the loop must feed — it now feeds.
  - `unitBusWaitBatchIdle()` waits `delay(1000)` + up to `REFLASH_BATCH_SETTLE_MS` (~16 s); combined with the trailing settle + reprobe in `runReflashJob` it can cross 30 s mid-reflash (strands a unit in twiboot). Feed inside the poll loop, and also feed before the trailing `unitBusProbe()` in `runReflashJob`.

### 5. Out of the subscribed set: OTA / AsyncTCP (deliberate)

The master OTA write (`Update.write()` in the `/firmware/master` `onUpload` callback, `WebEndpoints.cpp`) runs in the **AsyncTCP task context**, not netTask (a deliberate v1-precedent async exception, noted in-code). AsyncTCP is not one of the 5 subscribed tasks, so the OTA write is **not** directly WDT-watched. This is acceptable: a stuck OTA is already caught by #313's per-chunk 30 s stall watchdog (aborted from `webEndpointsLoop` in netTask), and AsyncTCP starvation still trips the CPU0 idle check. We do not subscribe or feed the AsyncTCP task.

## Resolved during cross-review (were open items)

1. **Arduino `loopTask`** does **not** need subscribing: `loop()` is only `tasksHeartbeatReport(); delay(5000)` — non-critical. `webEndpointsLoop()` runs in netTask (already subscribed), not loopTask.
2. **clusterTask** feeding is **unconditional** (per §4) — a single tick's fan-out can approach 30 s, so it is not left to measurement.

## Config guard (regression protection)

The 30 s timeout lives in the tracked `custom_sdkconfig`, but the *effective* value is in the generated (gitignored) `sdkconfig`. Add a pytest under `tests/` — same spirit as `test_partition_table.py` / `test_custom_bootloader.py` — that asserts `firmware/v2/Master/platformio.ini`'s `custom_sdkconfig` carries `CONFIG_ESP_TASK_WDT_TIMEOUT_S=30` (and that `PANIC=y` is not disabled), so a future edit can't silently drop the override. Runs in CI.

## Bench verification (E2E gate — via VPN board access)

1. Deliberately hang a subscribed task (behind a debug build flag) → board reboots within ~30 s; reset reason reported as TWDT.
2. Full master OTA → **no** false reboot. Specifically validates that netTask (core 0, subscribed) keeps getting scheduled to feed while AsyncTCP streams the image on the same core.
3. Full 16-unit reflash → no false reboot, no stranded unit (validates the per-batch feeding in `runReflashJob`).
4. mDNS discovery scan + cluster fan-out on the live wall → no false reboot (validates per-member feeding in `clusterLeaderTick`).

Native tests: the config-guard pytest above. No host test for the IDF glue itself (pure target API; hardware glue is bench-verified per project rule).

## Out of scope

- ESP-01 follower (`FollowerEsp01`) — single-core superloop on the ESP8266 SW watchdog (`ESP.wdtFeed()`), a different model. Follower logging visibility is #316.
- Rescue app — minimal, separate reliability posture.
