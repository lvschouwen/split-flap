# Boot-home inrush stagger + I²C heartbeat health model

**Date:** 2026-07-15
**Status:** design approved, pre-implementation (arc 2 — follows the headless-API/unit-vitals wave #306–#308)
**Trees touched:** `firmware/v1/Unit`, `firmware/v2/Master`, `firmware/v2/FollowerEsp01`

## Motivation

Two gaps surfaced while building the headless-observability wave:

1. **Power-up brownout (root cause of #305).** Every Nano shares the rail and
   powers up together, and each runs `calibrate(true)` at the end of `setup()`
   (`Unit.ino`) — so all 16 steppers **home simultaneously**. The current spike
   sags the rail and trips the S3's brownout detector on an OTA verify-boot,
   reverting good images. #305 only moved the OTA *confirm* out of the inrush's
   way; the mechanical spike itself is untouched. We are reflashing every unit
   for the vitals packet anyway — the moment to fix the boot behavior.

2. **v2 has no periodic unit-health refresh.** `unitBusPollHealth()` runs ONLY
   on an enqueued `Probe` (web `/units/health/refresh`, settings change, boot,
   reflash completion); `GET /units/health` serves the last snapshot. The v1
   ESP8266 master polled every ~60 s on its MQTT tick; the S3 port dropped it
   ("no bus polls from the MQTT loop"). So a curl-only operator sees **stale**
   health unless they manually refresh, and a unit that falls off the bus is
   never noticed proactively.

## Hard constraint (shapes part 2)

I²C slaves cannot initiate. The Nano is a slave; it physically cannot "call
back" the master, and there is no alert line to the S3. So a unit-pushed
heartbeat is impossible without new hardware. The **master drives every read**;
the heartbeat contract is a master-side schedule + miss detection, with a
symmetric unit-side "silence since master" awareness.

## Part 1 — Boot-home inrush stagger

### Unit side (`firmware/v1/Unit`)

`setup()` no longer calls `calibrate(true)`. The unit boots **UNHOMED / blank**,
records `bootMs`, and `loop()` homes on the FIRST of:

1. **`SFP_CMD_HOME` received** → home now (the master-orchestrated path).
2. **Any display/letter command while UNHOMED** → home *then* show (a driving
   master that skips an explicit HOME still gets a homed unit; the drum position
   is unknown until the first calibrate, so the first move must home anyway).
3. **No master contact within `SELF_HOME_TIMEOUT_MS` (30 s)** → address-staggered
   self-home at `30s + (addr-1)*STAGGER_MS + jitter(0..250ms)`. Standalone /
   no-master fallback. Jitter keeps coincident timing from re-synchronizing.
4. **Hard cap (`~120 s`)** — if a master IS seen but never commands a home
   (probe-only / old master), self-home anyway so the row is never permanently
   dark. The only case that overrides "wait for the master."

- "Master contact" = any I²C receive event (`receiveLetter` fired).
- The pre-home wait runs in `loop()`, which already kicks the 8 s watchdog; the
  boot-home deadline logic is pure and natively tested.
- `STAGGER_MS`, the hard cap and the jitter band are **bench-tuned using the
  #306 `vmin` telemetry** — home with different values, watch the rail floor.
- Backward compatible: a frozen/old master never sends batched HOME; the unit
  falls back to the staggered self-home at 30 s. No regression, just spread out.

### Master side (`firmware/v2/Master` + `firmware/v2/FollowerEsp01`)

After the boot bus-probe, orchestrate homing in **batches** rather than letting
the units time out together:

1. Probe the bus (already happens).
2. Command `SFP_CMD_HOME` to a batch of `N` units.
3. Wait for each to report **homed or faulted** (`CMD_GET_STATUS` moving flag),
   with a per-unit timeout — **status-driven, not a fixed sleep**.
4. Rail-settle delay (`300–1000 ms`) between batches.
5. Repeat.

- **Start at batch size `N=1`;** grow to 2–4 only once bench `vmin` proves the
  larger batch keeps the rail above the BOD threshold.
- Runs on both master generations (S3 displayTask; ESP-01 superloop).
- Pure sequencing/plan logic in a natively-tested header; the Wire glue stays in
  `UnitBus.cpp` / `FollowerBus.cpp` (sole Wire touchers).

## Part 2 — I²C heartbeat health model (master-driven TDMA)

Replace on-demand-only polling with a **scheduled heartbeat + miss detection**.

### Schedule (transport)

- Each unit occupies a slot in a repeating frame; `displayTask` reads **one unit
  per ~3 s tick** (round-robin), full fleet every ~48 s. Cadence is a tunable
  constant.
- **Opportunistic / low priority:** display position writes, reflash traffic and
  an explicit `Probe` always win. The heartbeat read is synthesized by
  `displayTask` only when otherwise idle, and is **skipped entirely during a
  reflash / twiboot window** (Hard rules: probe-inhibit deadline).
- `POST /units/health/refresh` stays as the explicit full-fleet immediate probe.

### Miss detection (the contract)

- Per unit, a **consecutive-miss counter**: a scheduled read that NACKs / fails
  checksum / times out increments it; a good read resets it.
- **≥K consecutive misses (default 3, tunable 3–5)** → the unit is flagged
  `stale`/`lost`: surfaced in the API (below) and raised as the HA fault sensor.
  This catches a unit that browned out / crashed / fell off the bus — which the
  current model never notices until someone refreshes.
- Pure counter/threshold logic in a natively-tested header (mirrors the existing
  `WearPolicy.h` / `UnitHealth.h` split).

### Unit-side autonomy (symmetric half)

- Each unit tracks **ms since the master last contacted it**. On prolonged
  silence it knows it is effectively standalone and can self-heal (hold, or
  re-verify home — the same muscle as Part 1's boot-home). Exposing the silence
  counter over I²C is optional (a reserved byte / vitals extension) and only if
  bench work wants it; the self-heal behavior is the valuable part.

## Part 3 — API surfacing (rides the #307/#308 headless surface)

- **`/units/health`** per unit (additive keys): `age` (ms since last good read),
  `stale` (1 when past the miss threshold), `misses` (consecutive), and a
  boot-home `hs2`/state field (`unhomed`/`homing`/`homed`).
- **`/cluster/health`** (follower): the same per-unit freshness where it serves
  the follower's row.
- **`/status`** includes the rolled-up freshness (headline oldest-age / any-stale).
- **`/api` legend**: every new terse key gets an entry. The arc-1 native
  **legend-completeness guard** (`test_api`) then FAILS until each new
  `buildUnitHealthJson` key is documented — the surface enforces its own docs.

## Backward compatibility & rollout

- One unit firmware covers vitals (#306) + boot-home (Part 1); staged + reflashed
  **once** at the final rev (the vitals-only intermediate rev is never flashed to
  hardware). `make_manifest.py stage` → both data dirs → CI drift gate.
- Old/frozen masters: units still self-home (staggered) at the 30 s fallback.
- Heartbeat poll is additive on the master; no unit change required for it (units
  already answer `CMD_GET_STATUS`).

## Testing

- **Unit (native):** boot-home decision table (which trigger fires when;
  timeout/stagger/jitter/hard-cap math) as a pure header.
- **Master/Follower (native):** batched-home sequencing plan; heartbeat schedule
  advance + consecutive-miss threshold; `/units/health` freshness keys + the
  legend-completeness guard (auto-covers the new keys).
- **Bench:** the payoff measurement — reflash units, watch `vmin` during a
  simultaneous vs. staggered/batched boot; confirm the S3 no longer BOD-reverts a
  verify-boot; pull a unit off the bus and watch the miss counter → `stale` →
  HA fault.

## Open decisions

- Exact `STAGGER_MS`, batch size growth, settle delay, miss threshold K, heartbeat
  cadence — all bench-tuned via `vmin`; defaults above are starting points.
- Whether to spend a vitals-packet byte on the unit's "silence since master"
  counter (deferred unless bench wants it).
