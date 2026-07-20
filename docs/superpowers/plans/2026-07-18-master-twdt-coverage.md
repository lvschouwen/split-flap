# Master TWDT App-Task Coverage (#314) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make a genuinely hung application task on the ESP32-S3 master auto-recover via a watchdog reboot, without false-tripping during legitimate long operations (unit reflash, OTA, cluster fan-out).

**Architecture:** Bump the ESP-IDF Task Watchdog (TWDT) timeout 5 s → 30 s via the tracked `custom_sdkconfig`; subscribe all 5 application FreeRTOS tasks with `esp_task_wdt_add(NULL)` and feed with `esp_task_wdt_reset()` at each loop top; feed *inside* long ops at progress points so a genuine wedge (e.g. an I2C driver hang) is still caught within ~30 s. The OTA write runs in the AsyncTCP task (not subscribed) and is left to the #313 stall-abort + CPU0 idle check.

**Tech Stack:** ESP-IDF `esp_task_wdt` API, Arduino-ESP32 (pioarduino hybrid), FreeRTOS, PlatformIO; pytest for the config-regression guard.

**Spec:** `docs/superpowers/specs/2026-07-18-master-twdt-coverage-design.md`

## Global Constraints

- Target `firmware/v2/Master` only. ESP-01 follower and Rescue are out of scope.
- Ships post-v2.0.0 → this is a **v2.0.1** reliability patch (or folded into a v2.1 "harden the wall" batch). Do NOT bump the version in any code/commit here.
- The 30 s timeout goes in `platformio.ini`'s `custom_sdkconfig` (tracked), NEVER the gitignored generated `sdkconfig.defaults`.
- Keep `CONFIG_ESP_TASK_WDT_PANIC=y` (timeout → reboot) and the existing CPU0 idle check. Do NOT enable the CPU1 idle check.
- Feed-inside, never unsubscribe: do not use `esp_task_wdt_delete()` around long ops (it would blind the dog to the I2C-wedge hang class #314 targets).
- Boot/reliability change → risk-tiered review always applies (cpp-reviewer over the combined branch diff). Bench is the E2E gate.
- Branch: `feat/master-twdt-314` (already created, carries the spec commits).
- Run all `pio`/`pytest` commands from `firmware/v2/Master/`.

## File Structure

- **Create** `firmware/v2/Master/TaskWatchdog.h` — two inline target-only helpers (`wdtSubscribeSelf()`, `wdtFeed()`). Dependency-free except `<esp_task_wdt.h>`/`<esp_err.h>`. One responsibility: wrap the TWDT subscribe/feed calls.
- **Create** `firmware/v2/Master/tests/test_task_watchdog_config.py` — pytest asserting `custom_sdkconfig` carries the 30 s timeout and PANIC stays on.
- **Modify** `firmware/v2/Master/platformio.ini` — add `CONFIG_ESP_TASK_WDT_TIMEOUT_S=30` to `custom_sdkconfig`; add an optional `-D TWDT_HANG_TEST` build flag comment for bench.
- **Modify** `firmware/v2/Master/Tasks.cpp` — subscribe + feed the 5 task loops; feed inside `runReflashJob` batches, the self-test poll, and boot-home stagger; optional hang-test hook.
- **Modify** `firmware/v2/Master/ClusterLeader.cpp` — feed inside `clusterLeaderTick`'s member fan-out and after the three service ticks.

---

### Task 1: Config timeout bump + regression guard (real TDD)

**Files:**
- Create: `firmware/v2/Master/tests/test_task_watchdog_config.py`
- Modify: `firmware/v2/Master/platformio.ini` (the `custom_sdkconfig =` block, currently line ~52)

**Interfaces:**
- Consumes: nothing.
- Produces: the guarantee that the built firmware runs the TWDT at 30 s with panic-reboot on.

- [ ] **Step 1: Write the failing test**

Create `firmware/v2/Master/tests/test_task_watchdog_config.py`:

```python
"""TWDT config guard (#314).

The effective ESP_TASK_WDT_TIMEOUT_S lives in the generated (gitignored)
sdkconfig; the tracked source is platformio.ini's custom_sdkconfig. This
test pins that source so a future edit can't silently drop the 30 s override
or the panic-reboot behaviour the unattended wall depends on.

Run with:
    pytest tests/
"""

from __future__ import annotations

import pathlib
import re

PROJECT_DIR = pathlib.Path(__file__).resolve().parent.parent
INI_PATH = PROJECT_DIR / "platformio.ini"


def custom_sdkconfig_lines() -> list[str]:
    """Return the entries of the [env:master] custom_sdkconfig block."""
    text = INI_PATH.read_text()
    m = re.search(r"^custom_sdkconfig\s*=\s*$(.*?)^\S", text,
                  re.MULTILINE | re.DOTALL)
    assert m, "custom_sdkconfig block not found in platformio.ini"
    lines = []
    for raw in m.group(1).splitlines():
        stripped = raw.strip()
        if stripped and not stripped.startswith(";"):
            lines.append(stripped)
    return lines


def test_task_wdt_timeout_is_30s():
    assert "CONFIG_ESP_TASK_WDT_TIMEOUT_S=30" in custom_sdkconfig_lines()


def test_task_wdt_panic_not_disabled():
    # PANIC defaults on (framework) and must not be turned off here — a
    # timeout must reboot, not just warn.
    assert "CONFIG_ESP_TASK_WDT_PANIC=n" not in custom_sdkconfig_lines()
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cd firmware/v2/Master && python -m pytest tests/test_task_watchdog_config.py -v`
Expected: `test_task_wdt_timeout_is_30s` FAILS (the line is not yet in `custom_sdkconfig`); `test_task_wdt_panic_not_disabled` passes.

- [ ] **Step 3: Add the timeout to `custom_sdkconfig`**

In `firmware/v2/Master/platformio.ini`, change the block from:

```
custom_sdkconfig =
  CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y
```

to:

```
custom_sdkconfig =
  CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y
  ; TWDT (#314): 30 s app-task watchdog. 5 s (framework default) is too tight
  ; for legitimate long ops; PANIC + CPU0 idle check stay as framework defaults.
  CONFIG_ESP_TASK_WDT_TIMEOUT_S=30
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `cd firmware/v2/Master && python -m pytest tests/test_task_watchdog_config.py -v`
Expected: both tests PASS.

- [ ] **Step 5: Commit**

```bash
git add firmware/v2/Master/tests/test_task_watchdog_config.py firmware/v2/Master/platformio.ini
git commit -m "feat(#314): 30 s TWDT timeout in custom_sdkconfig + regression guard"
```

---

### Task 2: `TaskWatchdog.h` helpers

**Files:**
- Create: `firmware/v2/Master/TaskWatchdog.h`

**Interfaces:**
- Consumes: `<esp_task_wdt.h>` (`esp_task_wdt_add`, `esp_task_wdt_reset`), `<esp_err.h>` (`esp_err_t`, `esp_err_to_name`).
- Produces:
  - `esp_err_t wdtSubscribeSelf();` — subscribe the calling task; returns the raw `esp_err_t` so the caller logs on failure.
  - `void wdtFeed();` — reset (feed) the TWDT for the calling task.

- [ ] **Step 1: Create the header**

Create `firmware/v2/Master/TaskWatchdog.h`:

```cpp
#pragma once
// ESP-IDF Task Watchdog helpers (#314). Target-only glue — no pure logic, so
// no native test; verified on the bench (a hung subscribed task must reboot
// within ~30 s, and OTA/reflash must NOT false-reboot). Feed-inside, never
// unsubscribe: keeping the dog live during long ops is the whole point (a
// wedged I2C transaction must still trip it).
#include <esp_err.h>
#include <esp_task_wdt.h>

// Subscribe the CALLING task to the TWDT. Returns ESP_OK on success; the
// caller logs esp_err_to_name(err) otherwise. Never aborts.
inline esp_err_t wdtSubscribeSelf() { return esp_task_wdt_add(nullptr); }

// Feed the TWDT for the calling task. Call at each task loop top AND at
// progress points inside long ops so no inter-feed span approaches 30 s.
inline void wdtFeed() { esp_task_wdt_reset(); }
```

- [ ] **Step 2: Verify it compiles (build the project)**

Run: `cd firmware/v2/Master && pio run`
Expected: build SUCCEEDS (header is unused so far but must parse/link).

- [ ] **Step 3: Commit**

```bash
git add firmware/v2/Master/TaskWatchdog.h
git commit -m "feat(#314): TaskWatchdog.h — TWDT subscribe/feed helpers"
```

---

### Task 3: Subscribe + feed the 5 task loops

**Files:**
- Modify: `firmware/v2/Master/Tasks.cpp` — `displayTaskMain` (subscribe before `for(;;)` at line ~460, feed at loop top), `clockTaskMain` (~797), `netTaskMain` (~908), `mqttTaskMain` (~926), `clusterTaskMain` (~941).

**Interfaces:**
- Consumes: `wdtSubscribeSelf()`, `wdtFeed()` (Task 2); `SerialPrintf` (already used throughout `Tasks.cpp`).
- Produces: all 5 tasks watched; each feeds once per loop iteration.

- [ ] **Step 1: Include the header**

At the top of `firmware/v2/Master/Tasks.cpp`, add with the other project includes:

```cpp
#include "TaskWatchdog.h"
```

- [ ] **Step 2: Subscribe + feed each task**

For EACH of the 5 task main functions, insert the subscribe call immediately before its `for (;;) {` loop, and the feed as the first statement inside the loop. Use the task's own name in the log string.

`displayTaskMain` — the `for (;;)` is at line ~460, after the boot probe/auto-install block:

```cpp
  if (esp_err_t e = wdtSubscribeSelf(); e != ESP_OK)
    SerialPrintf("wdt: display subscribe -> %s\n", esp_err_to_name(e));
  for (;;) {
    wdtFeed();
    // ... existing loop body ...
```

`clockTaskMain` (~797):

```cpp
  if (esp_err_t e = wdtSubscribeSelf(); e != ESP_OK)
    SerialPrintf("wdt: clock subscribe -> %s\n", esp_err_to_name(e));
  for (;;) {
    wdtFeed();
```

`netTaskMain` (~908):

```cpp
  if (esp_err_t e = wdtSubscribeSelf(); e != ESP_OK)
    SerialPrintf("wdt: net subscribe -> %s\n", esp_err_to_name(e));
  for (;;) {
    wdtFeed();
```

`mqttTaskMain` (~926):

```cpp
  if (esp_err_t e = wdtSubscribeSelf(); e != ESP_OK)
    SerialPrintf("wdt: mqtt subscribe -> %s\n", esp_err_to_name(e));
  for (;;) {
    wdtFeed();
```

`clusterTaskMain` (~941):

```cpp
  if (esp_err_t e = wdtSubscribeSelf(); e != ESP_OK)
    SerialPrintf("wdt: cluster subscribe -> %s\n", esp_err_to_name(e));
  for (;;) {
    wdtFeed();
    clusterLeaderTick();
    vTaskDelay(pdMS_TO_TICKS(100));
  }
```

- [ ] **Step 3: Build + confirm native tests still green**

Run: `cd firmware/v2/Master && pio run && pio test -e native`
Expected: build SUCCEEDS; native suite PASSES (no regression — this is target-only glue, ArduinoFake/native doesn't exercise it).

- [ ] **Step 4: Commit**

```bash
git add firmware/v2/Master/Tasks.cpp
git commit -m "feat(#314): subscribe + feed all 5 app tasks to the TWDT"
```

---

### Task 4: Feed inside displayTask long ops

**Files:**
- Modify: `firmware/v2/Master/Tasks.cpp` — `runReflashJob` (def line ~278), the boot-home stagger loop (`for (int j = i; ...)` at line ~207), the self-test poll path (~616).

**Interfaces:**
- Consumes: `wdtFeed()` (Task 2).
- Produces: no inter-feed span in displayTask exceeds ~30 s during reflash / self-test / boot-home; a genuine wedge still trips the dog.

- [ ] **Step 1: Feed per boot-home batch**

In the boot-home stagger loop in `Tasks.cpp` (the `for (int i = 0; i < n; ...)` batch loop around line 205, which commands a batch then waits for each unit to settle), add a feed at the top of each batch iteration:

```cpp
  for (int i = 0; i < n; /* advanced below */) {
    wdtFeed();  // #314: each batch waits on unit settle — keep the dog fed
    int batchN = 0;
    for (int j = i; j < n && batchN < BOOT_HOME_BATCH_SIZE; j++) {
      // ... existing ...
```

- [ ] **Step 2: Feed per reflash step in `runReflashJob`**

`runReflashJob` (line ~278) streams unit hex in batches over I2C for tens of seconds. Add a `wdtFeed();` at the top of its main per-batch / per-unit loop body (the loop that iterates the units being flashed), and one after its concluding boot-home call (~line 449–453):

```cpp
    // top of the per-unit / per-batch reflash loop body:
    wdtFeed();  // #314: I2C page-streaming is the longest displayTask op

    // ... existing per-unit flash work ...
```

```cpp
    // the reflash-ending boot-home (~449):
    wdtFeed();  // #314: boot-home of just-flashed units
    runReflashJob-local boot-home ...  // (keep existing call)
```

- [ ] **Step 3: Feed around the self-test poll**

The self-test path (~line 616, where displayTask polls a unit's self-test outcome) can loop waiting on the unit. Add a `wdtFeed();` inside that poll loop:

```cpp
    // inside the self-test poll/wait loop:
    wdtFeed();  // #314: self-test polls the unit until it reports an outcome
```

- [ ] **Step 4: Build + confirm native tests still green**

Run: `cd firmware/v2/Master && pio run && pio test -e native`
Expected: build SUCCEEDS; native suite PASSES.

- [ ] **Step 5: Commit**

```bash
git add firmware/v2/Master/Tasks.cpp
git commit -m "feat(#314): feed the TWDT inside displayTask long ops (reflash, self-test, boot-home)"
```

---

### Task 5: Feed inside the cluster leader tick

**Files:**
- Modify: `firmware/v2/Master/ClusterLeader.cpp` — `clusterLeaderTick()` (def line ~1346): the member fan-out loop (~1385) and after `rolloutServiceTick()` / `followerPushServiceTick()` / `followerLogPullTick()` (~1408–1414).

**Interfaces:**
- Consumes: `wdtFeed()` (Task 2).
- Produces: a single tick's multi-member fan-out plus the three service ticks never span > 30 s unfed; the reboot-hold path already returns one-member-per-tick and is covered by the loop-top feed (Task 3).

- [ ] **Step 1: Include the header**

At the top of `firmware/v2/Master/ClusterLeader.cpp`, add with the other project includes:

```cpp
#include "TaskWatchdog.h"
```

- [ ] **Step 2: Feed after each member and each service tick**

In `clusterLeaderTick()`, the normal fan-out loop (line ~1385) does one blocking `clusterHttpRequest` (1.5 s timeout) per member. Feed after each, and after each of the three trailing service ticks:

```cpp
  for (int i = 0; i < count; i++) {
    String body;
    int status = clusterHttpRequest(items[i].url, items[i].body, body);
    applyMemberResult(items[i], status, body);
    wdtFeed();  // #314: up to CLUSTER_MAX_MEMBERS blocking sends in one tick
  }

  rolloutServiceTick();
  wdtFeed();            // #314: a #276 firmware chunk write can block
  followerPushServiceTick();
  wdtFeed();            // #314: #304 relay chunk write
  followerLogPullTick();
  wdtFeed();            // #314: #318E log poll
```

(The reboot-hold `clusterHttpRequest` earlier in the tick already `return`s after one op, so the loop-top feed in Task 3 covers it — no extra feed needed there.)

- [ ] **Step 3: Build + confirm native tests still green**

Run: `cd firmware/v2/Master && pio run && pio test -e native`
Expected: build SUCCEEDS; native suite (incl. `test_cluster_leader_policy`) PASSES.

- [ ] **Step 4: Commit**

```bash
git add firmware/v2/Master/ClusterLeader.cpp
git commit -m "feat(#314): feed the TWDT across the cluster leader tick fan-out"
```

---

### Task 6: Bench hang-test hook + verification

**Files:**
- Modify: `firmware/v2/Master/platformio.ini` (`[env:master]` `build_flags`, ~line 100) — document an optional `-D TWDT_HANG_TEST` flag.
- Modify: `firmware/v2/Master/Tasks.cpp` — a compile-guarded hang injection at the top of `displayTaskMain`'s loop.

**Interfaces:**
- Consumes: nothing new.
- Produces: a build-flag-gated way to prove the reboot on the bench, compiled out of shipping builds.

- [ ] **Step 1: Add the compile-guarded hang hook**

In `displayTaskMain`, immediately after the `wdtFeed();` at the loop top (Task 3), add:

```cpp
#ifdef TWDT_HANG_TEST
    // #314 bench: after 20 s of normal running, wedge displayTask forever so
    // the TWDT must reboot within ~30 s. NEVER defined in a shipping build.
    if (millis() > 20000) { for (;;) { /* no wdtFeed() → dog fires */ } }
#endif
```

- [ ] **Step 2: Document the flag in platformio.ini**

Under `[env:master]` `build_flags` (~line 100), add a commented line (kept OFF):

```
  ; #314 bench only — wedge displayTask ~20 s after boot to prove the TWDT
  ; reboot. Never enable in a shipping/OTA build. Uncomment to bench:
  ; -D TWDT_HANG_TEST
```

- [ ] **Step 3: Build the normal (flag-off) image and confirm it's clean**

Run: `cd firmware/v2/Master && pio run && pio test -e native`
Expected: build SUCCEEDS (hang code compiled out); native PASSES.

- [ ] **Step 4: Commit**

```bash
git add firmware/v2/Master/platformio.ini firmware/v2/Master/Tasks.cpp
git commit -m "test(#314): compile-guarded TWDT hang hook for bench verification"
```

- [ ] **Step 5: cpp-reviewer gate (risk-tiered — always for boot/reliability)**

Run the cpp-reviewer agent over the combined branch diff (`git diff master...feat/master-twdt-314`). Address CRITICAL/HIGH; fix MEDIUM where reasonable.

- [ ] **Step 6: Bench verification (E2E gate — via VPN board access)**

Stage the normal image, OTA it to the bench S3, and run:
1. Build once WITH `-D TWDT_HANG_TEST`, flash → confirm the board reboots ~30 s after boot with a TWDT/panic reset reason in the boot banner. Re-flash the clean (flag-off) image after.
2. Full master OTA → **no** false reboot (validates netTask keeps feeding while AsyncTCP streams on the same core).
3. Full 16-unit reflash (`/reflash-units`) → no false reboot, no unit stranded in twiboot.
4. Trigger an mDNS discovery scan + cluster fan-out on the live wall → no false reboot.

- [ ] **Step 7: Open the PR**

```bash
git push -u origin feat/master-twdt-314
gh pr create --base master --title "reliability(v2): TWDT app-task coverage (#314)" \
  --body "Implements docs/superpowers/specs/2026-07-18-master-twdt-coverage-design.md. Closes #314. Bench-verified: hang→reboot, OTA/reflash/cluster no false reboot."
```

---

## Self-Review

**Spec coverage:**
- Timeout 30 s in `custom_sdkconfig` → Task 1. ✅
- Subscribe all 5 tasks + loop-top feed → Task 3. ✅
- Feed-inside long ops (reflash, self-test, boot-home) → Task 4. ✅
- Cluster per-member + per-service-tick feed → Task 5. ✅
- OTA/AsyncTCP left unwatched (deliberate) → covered by not touching it; noted in plan architecture. ✅
- Error-code handling (ESP_OK vs log) → Task 2 returns `esp_err_t`, Task 3 logs `esp_err_to_name`. ✅
- loopTask NOT subscribed → not in the task list; correct by omission. ✅
- Config-guard pytest → Task 1. ✅
- `TaskWatchdog.h` target-only, no native test → Task 2. ✅
- Bench gate (hang, OTA, reflash, cluster) → Task 6. ✅
- No CPU1 idle check → not added anywhere. ✅

**Placeholder scan:** No TBD/TODO; every code step shows the exact insertion. Line numbers are marked "~" because they will drift as edits land — each is anchored to a named function or existing code snippet the implementer greps for.

**Type consistency:** `wdtSubscribeSelf()` returns `esp_err_t` (Task 2) and is consumed as `esp_err_t e = wdtSubscribeSelf()` (Task 3). `wdtFeed()` returns void, called as a statement everywhere. Consistent.
