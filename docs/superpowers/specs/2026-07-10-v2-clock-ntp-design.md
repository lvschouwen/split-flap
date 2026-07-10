# v2 clock/NTP slice — design (#192, #58 slice 6)

**Date:** 2026-07-10 · **Status:** approved (decisions locked in the 2026-07-10 overnight brief: v1
parity, `configTzTime` with `settings.timezonePosix`, clockTask on core 1 owns the 1 Hz tick,
display writes as `DisplayCommand`s only)

## What v1 does (the parity contract)

- `applyTimezoneAndNtp()` (`ServiceSettingsFunctions.ino`): effective TZ = runtime EEPROM setting →
  compile-time `timezonePosix` const → `"UTC0"`; NTP server = compile-time `timezoneServer` const →
  `"pool.ntp.org"`. Called once after WiFi join in `setup()` and again from the settings drain when
  the web UI changes the timezone (#48: no reboot needed).
- `formatDateTime(fmt)` (`HelpersStringHandling.ino`): `time()` → `localtime_r` → `strftime`.
- `loop()` ticks once per second: mode `text` → `showText(inputText)`, mode `clock` →
  `showText(formatDateTime(clockFormat))` with `clockFormat = "%H:%M"`. `showText()` dedups on
  `lastWrittenText`, so the per-second re-show costs no I2C traffic unless the content changed.
- `setup()` blocks up to 10 s for `time() >= 1000000000` after `configTime` before first display
  traffic.
- Message/mode POSTs stamp `lastReceivedMessageDateTime` at the drain.

## v2 design

### Pure logic — `ClockPolicy.h` (natively tested, new `test_clock_policy`)

- `clockIsTimeSynced(time_t)` — `> 1000000000` (v1's boot-wait constant).
- `formatDateTime(time_t, fmt)` — `localtime_r` + `strftime` into a fixed 64-byte buffer, returns
  `String`. Takes the time explicitly (testable with a fixed epoch + `setenv("TZ")`/`tzset()` on the
  native host); the glue passes `time(nullptr)`.
- `decideClockTick(in) -> ClockTickDecision {enqueue, text}` — the whole ticker brain:

  | mode | inputs | decision |
  |---|---|---|
  | `clock`, synced | formatted time ≠ dedup state | enqueue formatted time |
  | `clock`, NOT synced | — | nothing (hold last content; deliberate deviation — v1 would flap epoch-1970 garbage if NTP never syncs) |
  | `text` | inputText ≠ dedup state (incl. empty→nothing) | enqueue inputText — this is what re-shows the message on a clock→text mode switch |
  | any | display busy | nothing (defer to next tick) |

  Dedup state = `{snapshotCurrentText, lastQueuedByTicker}`: enqueue only if desired text differs
  from **both**. `lastQueuedByTicker` covers the queue-latency window (command sent, worker not yet
  done — snapshot still stale); it is cleared once `snapshotCurrentText` catches up, so the ticker
  can legitimately re-show a text the display has since moved away from. Empty desired text never
  enqueues (boot with no message = blank display, v1 parity).

  **Domain rule (review H1):** every text entering these comparisons must be display-domain —
  passed through `truncateForDisplay()` (`DisplayCommand.h`, the exact projection
  `makeShowTextCommand` applies). The retained `inputText` is truncated at the drain when stored;
  a raw >16-char string could never equal any snapshot text and would wedge `lastQueuedByTicker`
  permanently.

### Glue — `ClockService.h/.cpp`

- `clockServiceApplyTz(const MasterSettings&)`: `configTzTime(tz, "pool.ntp.org")` with
  `tz = settings.timezonePosix` (loadSettings guarantees validity; belt-and-braces empty → `"UTC0"`).
  NTP server stays a compile-time const like v1 (no settings knob).
- Call sites (both netTask context, matching v1's loop()-context rule):
  - `WifiService.cpp startOnline()` — first join. Portal-only boots get no NTP (no upstream) —
    clock mode simply stays gated on sync.
  - `WebEndpoints.cpp` drain — when `applySettingsPost` changed `timezonePosix` (compare before /
    after), re-apply immediately. Thread-safety: ESP-IDF newlib wraps the TZ globals in
    `__tz_lock()` — `configTzTime` on core 0 vs `localtime_r` on core 1 is safe.
- No 10 s boot block: tasks are free-running; the sync gate in the ticker does the waiting.

### clockTask (`Tasks.cpp`) — stub becomes the real ticker

Per tick (1 Hz, `vTaskDelayUntil` as today):

1. `webDisplayContentSnapshot()` — new accessor in `WebEndpoints.cpp`: copies
   `{deviceMode, inputText, alignment, flapSpeed}` under `WebStateLock`. `inputText` is new
   **runtime-only** TU-state in WebEndpoints (never persisted, boots `""` — v1 parity), recorded
   **display-domain truncated** at the drain when a message POST lands — and only when the
   effective mode (after any mode field on the same POST) is `text`, matching v1's
   `if (deviceMode == DEVICE_MODE_TEXT)` gate: a message posted while in clock mode is silently
   ignored, never shown or retained (review M1).
2. `displaySnapshotGet()` — existing mutex copy.
3. `decideClockTick(...)` with `formatDateTime(time(nullptr), CLOCK_FORMAT)`;
   `CLOCK_FORMAT = "%H:%M"` compile const (v1 `clockFormat`).
4. On enqueue: `makeShowTextCommand(text, alignment, flapSpeed)` → `displayEnqueue()` (parameters
   baked in by the sender per the #187 contract). Queue-full → log, retry next tick (dedup state
   only advances on successful enqueue).

Costs: clock mode = one command per minute; text mode = zero steady-state commands. The drain still
enqueues message POSTs directly (immediate response); the ticker's dedup makes the overlap benign —
worst case one redundant same-text command in the 1-tick latency window.

### Ride-alongs

- `lastTimeReceivedMessageDateTime` (already in `/settings` JSON, always `""` until now): stamped at
  the drain via `formatDateTime(time(nullptr), "%d %b %y %H:%M:%S")` when a message or mode POST
  lands (v1 stamps message/mode submissions only, not settings saves — #128 parity). Kept in the
  same WebEndpoints TU-state as `inputText`.

## Out of scope

Transient/MQTT notification dwell (mode service, later slice); date mode (v1 has none); persisted
NTP server; recovery/quiet-boot interactions (later slice).

## Test plan

- `test_clock_policy` (native): sync gate boundary; clock-mode minute rollover (enqueue once, then
  silence until the string changes); unsynced clock holds; text-mode re-show on mode switch;
  busy defers; empty text never enqueues; in-flight dedup (lastQueued) including the
  clear-on-catch-up path; queue-full retry semantics (dedup only advances on success —
  covered by decision function returning desired text idempotently).
- `formatDateTime` native tests with fixed epochs + `TZ=CET-1CEST,M3.5.0,M10.5.0/3` (DST both sides)
  and `%H:%M` / v1 stamp format.
- Existing suites stay green; S3 build; bench (devkit): boot → join → `/settings` shows stamped
  time after a POST; mode `clock` via web UI → console shows one ShowText per minute with the
  correct local time.
