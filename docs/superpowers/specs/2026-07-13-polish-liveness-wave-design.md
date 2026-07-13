# Polish + liveness wave (post-#247)

Bench feedback from the 6e26e2d flash session, 2026-07-13. Six independent
slices, one branch/PR. All firmware work is v2 Master unless noted.

## 1. ota-flash.sh hardening + OTA throughput measurement

Observed: an upload died at 55% with `curl: (56) Recv failure`, and the
script reported `HTTP 100000` + "upload rejected" — a network abort
misdiagnosed as a device rejection. A manual re-run succeeded.

Script (`flashing/ota-flash.sh`):
- Send `-H 'Expect:'` on the upload POST so curl never emits
  `Expect: 100-continue` (the stray leading `100` in `100000`).
- Check curl's exit code before interpreting the HTTP code. Non-zero exit
  with code 7/28/52/55/56 = network-level failure: report it as such and
  **auto-retry once** (the device's `Update.begin` path already aborts a
  stale half-session, verified in this bench). Any other non-zero exit or
  a real HTTP error keeps the current single-shot verdict behavior.

Firmware (`WebEndpoints.cpp` master OTA handler):
- Stamp `millis()` at upload start (index == 0), log
  `bytes / elapsed ms / KB/s` on the final chunk. This is the measurement
  that decides whether flash erase dominates upload wall time.

Parked pending those measurements (noted on the issue, not built now):
**erase-ahead** — background-erase the inactive slot after confirm and
write with raw `esp_partition_write` (FactorySlot.cpp precedent), skipping
per-sector erase. Costs the standing second app image + needs an
erased-flag discipline in NVS; only worth designing if erase dominates.
Alternative if the *network* dominates: pull-mode OTA (device fetches from
the build server). LittleFS staging was considered and rejected: same
flash chip so no speed win, double wear, and it violates the
netTask-sole-writer rule for `storage`.

## 2. Web UI polish bundle

- Boot banner (`main.cpp`): `split-flap v2 master — Phase 1 (#58)` →
  `split-flap v2 master — <GIT_REV>` (macro string-literal concat; #58 is
  long closed).
- System tab CPU tiles (`data/index.html`): `CPU net (core 0)` →
  `CPU core 0`, `CPU display (core 1)` → `CPU core 1` (user preference).
- Heap tile honesty: `statMaxAlloc` (ESP.getMaxAllocHeap = largest
  contiguous block of the *internal* heap) currently renders as the PSRAM
  tile's sub-line, implying 8 MB PSRAM is fragmented to 124 KB. Move it
  under **Free heap** labeled "largest contiguous"; the PSRAM tile keeps
  no sub-line.
- Health strip during reflash (`data/script.js` + `style.css`): the strip
  marks any unit with `st !== 1` as `bad` (red), so a healthy reflash
  paints all 16 red. While `reflashIsRunning(rf)`, bootloader-mode units
  get a new `flashing` class (yellow); the unit at `rf.cur` pulses.
  `pollReflashProgress` currently only re-renders unit health when the job
  ends — during the job it must also update the strip from the same 2 s
  `/units/health` response.

## 3. Reflash batch size 2 → 4

`REFLASH_BATCH_SIZE` exists as a brownout throttle (v1 #138: post-flash
homing inrush on the shared supply). The I2C flashing itself is serial;
the batch only limits *simultaneous homing*, so the cost of batch=2 is the
8 settle-waits per full display. Bump to 4 (→ 4 settle-waits).

Bench gate: after a full 16-unit reflash at batch=4, the per-unit lifetime
brownout counters (#139, unit health table) must not have incremented.
If they did, revert to 2 — the counters are the instrument #138 lacked.
Pipelining (flash batch N+1 while batch N homes) is explicitly out of
scope unless batch=4 proves insufficient.

## 4. Live updates: SSE mirror + riffle animation + dual-rate vitals

Today the flap mirror rides the 5 s `/settings` poll and the vitals
sampler only takes a sample every 5 s, so both feel laggy.

**SSE channel** (`WebEndpoints.cpp` + netTask): `AsyncEventSource` at
`GET /events`. netTask's tick compares the mutex-copied display snapshot's
`lastWrittenText` (+ alignment) against the last value it pushed and sends
a `display` event with a small JSON payload on change; `onConnect` sends
the current state so a fresh client paints immediately. Sends happen from
netTask only — consistent with the async-context rules in the
WebEndpoints.cpp header (the event source queues per-client). Browser:
`EventSource` with automatic reconnect; the existing 5 s `/settings` poll
stays as-is and doubles as the fallback path, so a dropped stream degrades
to today's behavior, never worse.

**Riffle animation** (`data/script.js`/`style.css`, client-only): a tile
no longer folds once from old glyph to new — it steps *through* the
alphabet (the `ALPHABET` mirror of `SFP_ALPHABET`, forward-only with
wrap-around, matching the hardware's one-way drum) at a rate derived from
the configured speed, reusing the existing two-leaf fold per step. Rate
capped so a worst-case 44-flap travel stays under ~3 s.

**Dual-rate vitals** (`SystemStatsPolicy.h` + `SystemStats.cpp`): sampler
tick drops from 5 s to 1 s; every sample refreshes the `now` block, but
only every 5th sample is pushed into the history ring (decimation in the
pure policy, natively tested), so the sparklines keep their ~10 min depth
while the tiles become effectively live through the UI's existing 2 s
poll. CPU% window becomes 1 s, which is fine for the idle-delta method.

## 5. Full timezone list with type-ahead

The 14-entry curated dropdown is an ESP-01 flash-budget fossil. Replace
with the full posix_tz_db list (~460 zones):

- Vendor `zones.csv` (nayarsystems/posix_tz_db) into the repo next to the
  other web sources; `build_assets.py` converts it to gzipped JSON baked
  into `WebAssets.h` as a new ASSETS entry, served at `GET /tz.json`.
- UI: replace the `<select>` with a text input + filtered suggestion list
  (type-ahead over IANA names). Display value = IANA name, submitted
  value = POSIX string — the `/settings` wire contract is unchanged.
  `tz.json` is fetched lazily the first time the Settings tab opens.
- A stored POSIX string not in the table still displays (current
  fallback behavior preserved).

## 6. PCB v2 docs: INA226 rail monitoring

The S3 has no on-chip supply V/A sensing; brownout is currently inferred
from unit-side reset counters. Add an INA226 current-shunt monitor on the
supply rail to the PCB v2 master board design notes (I2C, feeds a future
System tab tile; would have made #138 a measurement instead of a
diagnosis). Docs only — no firmware.

## Test plan

- Native: new tests for stats decimation policy; updated
  `test_reflash_plan` for batch=4; existing suites stay green.
- Script: extend the ota-flash.sh mock-device tests with a
  connection-reset-then-retry scenario.
- Bench (user): reflash at batch=4 watching brownout counters; SSE mirror
  flips in step with the hardware; next OTA prints a KB/s line and the
  script survives a mid-upload reset; timezone picker round-trips.
