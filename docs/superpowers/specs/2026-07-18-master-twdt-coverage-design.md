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

### 3. RAII pause guard — new header `TaskWatchdog.h`

A scope object that unsubscribes the calling task from the TWDT for the duration of a long, self-bounded op, then re-subscribes:

- **ctor:** `esp_task_wdt_delete(NULL)`; record whether it was actually subscribed (delete returned `ESP_OK`).
- **dtor:** `esp_task_wdt_add(NULL)` **only if** the ctor's delete succeeded — safe against early returns / exceptions, and never double-subscribes.

Target-only IDF glue. No pure logic → no native test (consistent with the project rule: host tests cover pure logic only, hardware glue is bench-verified).

Rationale for unsubscribe-vs-feed: every long op below is **already individually time-bounded** by its own timeout (I2C hardware transaction timeouts; OTA's #313 per-chunk 30 s stall watchdog; cluster HTTP 1.5 s `esp_http_client` timeouts; `MDNS.queryService` timeout arg). The TWDT's job is to catch the *unbounded* hangs (event-loop deadlock, unreleased mutex, never-waking queue), which occur in the normal loop flow — not inside these bounded ops. So pausing the dog around a bounded op loses no meaningful protection while making false-reboot avoidance a one-line guard.

### 4. Guard sites

Wrap a `WdtPause` around each long op:

- **`runReflashJob`** (displayTask) — streams unit hex over I2C for tens of seconds; the critical one, a false reboot strands a unit in twiboot.
- **self-test poll loops** and **boot-home `delay()`s** (displayTask).
- **master OTA flash-write path** and **`MDNS.queryService` scan** (netTask).
- **cluster #276 firmware-stream / finalize** (clusterTask) — guard **only if** measurement shows a single tick's fan-out can approach 30 s; otherwise each tick returns to the loop (and its `esp_task_wdt_reset()`) well within budget.

### 5. Error handling

`esp_task_wdt_add` returning `ESP_ERR_INVALID_STATE` (already subscribed) is logged and ignored — never fatal. The guard's conditional re-add prevents double-subscribe.

## Open implementation items (resolved during build, not blockers)

1. **Arduino `loopTask`** runs `webEndpointsLoop()` (incl. #313's OTA stall-abort). Determine whether the framework already WDT-subscribes `loopTask`; if it does real work unwatched, subscribe it as a 6th watched task (same add-at-start / reset-at-top pattern).
2. **clusterTask worst-case tick** duration → decides whether guard site #4's cluster entry is needed.

## Bench verification (E2E gate — via VPN board access)

1. Deliberately hang a subscribed task (behind a debug build flag) → board reboots within ~30 s; reset reason reported as TWDT.
2. Full master OTA → **no** false reboot mid-flash.
3. Full 16-unit reflash → no false reboot, no stranded unit.
4. mDNS discovery scan + cluster fan-out on the live wall → no false reboot.

Native tests: **not applicable** (pure IDF glue; no host-testable logic).

## Out of scope

- ESP-01 follower (`FollowerEsp01`) — single-core superloop on the ESP8266 SW watchdog (`ESP.wdtFeed()`), a different model. Follower logging visibility is #316.
- Rescue app — minimal, separate reliability posture.
