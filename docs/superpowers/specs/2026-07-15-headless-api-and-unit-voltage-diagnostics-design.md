# Headless observability API + unit voltage diagnostics

**Date:** 2026-07-15
**Status:** design approved, pre-implementation
**Trees touched:** `firmware/v1/Unit`, `firmware/v2/Master`, `firmware/v2/FollowerEsp01`

## Motivation

The operator works **curl-only, no browser**. The web UI is just a renderer of
existing JSON endpoints, so parity already exists for what those endpoints
carry — but three gaps make a headless workflow harder than it should be, and
one of them cost a full multi-flash bisection this session:

1. **No route/field discovery** — you must read source or the browser to learn
   what endpoints exist and what the terse `/units/health` keys mean.
2. **The ESP-01 follower is opaque** — no `/system/stats`; you can't see *why* a
   row is blank/stale (time since last leader render, blank countdown), its I²C
   bus health, min-heap, or SNTP state.
3. **No supply-voltage visibility** — the #305 brownout saga (good OTA images
   A/B-reverting on a boot sag) was diagnosed by elimination. Had each Nano
   reported its own Vcc, the sag would have been one curl.

This wave closes all three, plus a static inventory endpoint and a one-shot
aggregate, and extends the Unit I²C protocol so units report their supply
voltage (sampled mid-move, where the sag lives).

## Scope

**In:** unit Vcc + commanded-position + free-RAM diagnostics over a new
checksummed I²C read; master `/api`, `/system/info`, `/status`; follower `/api`
and diagnostics folded into `/cluster/health`; per-unit + headline Vcc in
`/units/health` on both master generations; field legends for all terse
endpoints; native tests; bundle re-stage + unit reflash.

**Out:** stall/missed-step counter (cut — 28BYJ is open-loop, single hall
reference per rev; the only honest signal is homing-step deviation, already
exposed as drift `de`/`ds`, #263). Lifetime (EEPROM) Vcc-min (since-boot RAM
only — the lifetime brownout count already persists and pairs with it). Any
MQTT/control additions to the ESP-01. v1 `ESPMaster` (frozen #283 — will not
parse the new packet; units stay backward-compatible).

## 1. Unit firmware — I²C diagnostics packet

New read command `CMD_READ_DIAG` (next free opcode in `UnitI2CProtocol`, must
not collide with existing commands or twiboot's range). Returns a **checksummed
fixed packet** — the exact backward-compatibility pattern the odometer read
(#231) already uses:

```
offset  field         type   notes
0..1    vccNow_mV     u16 LE  AVR bandgap read, current
2..3    vccMin_mV     u16 LE  since-boot minimum (unit samples mid-move)
4       cmdPos        u8      last commanded flap index (0..FLAP_AMOUNT-1)
5..6    freeRamMin    u16 LE  since-boot minimum free SRAM (bytes)
7       checksum      u8      same scheme as the odometer reply
```

- **Vcc** measured via the internal 1.1 V bandgap referenced to AVcc
  (`ADMUX = (1<<REFS0)|0b1110`; `Vcc_mV = 1100L*1023/ADC`). The conversion is a
  pure function (`unitVccFromAdc(uint16 adc)`) in a natively-tested header.
- **vccMin** is sampled **during motion** (once per move, near peak coil load)
  so it captures the sag, not the idle rail. Held in RAM; a brownout/reboot
  resets it (the persisted brownout *count* records that the event happened).
- **cmdPos** is the unit's own copy of the last commanded target letter index.
  Physical position is NOT duplicated here — it already rides the drift block
  (`phys`, hall-synced).
- **freeRamMin** tracks the lifetime-low free SRAM between heap top and stack.

Packet build/parse logic lives in a pure header (mirroring `UnitOdometer.h`),
natively tested in the Unit dir. Un-reflashed units NACK/garbage the unknown
command → checksum fails → the reader sets `diagV2 = false` and emits no Vcc
fields (never garbage), exactly like a pre-odometer unit.

## 2. Master (ESP32-S3) — endpoints

### `GET /api`
Self-documenting index: `{"routes":[{"m":"GET","p":"/settings","d":"…"},…],
"legend":{…}}`. `routes` covers every registered endpoint (method, path,
one-line description). `legend` maps the terse keys of every terse endpoint
(`/units/health`, `/system/stats`, `/cluster/status`) to human meanings. Body
is a static PROGMEM string. A native test asserts every key `buildUnitHealthJson`
emits has a legend entry, so the legend can't silently drift from the data.

### `GET /system/info`
Static inventory: chip model/rev/cores/cpuMHz, flashKB, psramKB, partition
layout (label/subtype/offset/size), `rev` (GIT_REV), `bundledUnitRev`
(BUNDLED_UNIT_REV), `bootCount`, `resetReason`, `factoryValid`, `rescueValid`.
Reuses existing accessors; no new sampling.

### `GET /status`
One-shot aggregate: `{"settings":…,"stats":…,"units":…,"cluster":…,"ota":…}`,
composed from the existing serializers. Includes `stats.now` **only** (the
~10-min history ring stays at `/system/stats` to keep `/status` bounded).

### `/units/health` additions
Per unit, behind `diagV2` (present only when the diag packet validated):
`"vcc"` (mV now), `"vmin"` (mV since-boot min), `"cp"` (commanded flap index),
`"ram"` (free-SRAM min bytes). Headline gains **`"vccMin"`** = `min(vmin)` across
all valid units — the worst rail any unit has seen since boot, i.e. the
brownout smoking gun.

## 3. ESP-01 follower — endpoints

### `GET /api`
Same self-documenting shape, its trimmed route subset + legends for its terse
endpoints (`/units/health`, `/cluster/health`).

### `/cluster/health` additions (follower diagnostics)
Existing: `state`, `leaderName`, `leaderHost`, `row`, `epoch`, `seq`,
`segment`, `rev`, `width`, `detected`, `faulty`. Add: `msSinceRender`
(since the last leader render applied), `secsUntilBlank` (countdown to the
~2-min blank; the follower already tracks this in `FollowerPolicy`), `i2cTx`,
`i2cErr` (bus counters), `minHeap`, `sntpSynced` (commitAt readiness). `state`
already IS the phase (Standalone/Clustered/Grace/Blank) — no separate field.

### `/units/health` additions
Same per-unit Vcc fields as the master (the follower's `FollowerBus` issues
`CMD_READ_DIAG` and parses the same packet via the shared pure header).

## 4. Field legends

Legends are static PROGMEM key→meaning maps embedded in each board's `/api`
`legend` object, covering all terse endpoints on that board. Cheap; being
thorough (all terse endpoints, not just `/units/health`) is preferred over
minimal. The copied pure legend/field definitions must match between trees —
same fix-in-both-trees rule as the other copied pure headers.

## 5. Backward compatibility & rollout

- Units on old fw (31775b5) → `diagV2=false`, no Vcc fields. Fully functional.
- New unit fw must be **bundled + reflashed**: change unit code → rebuild Unit
  clean → `make_manifest.py stage` (writes the hex into BOTH `firmware/v2/Master/data`
  and `firmware/v2/FollowerEsp01/data`) → build Master + Follower → **separate
  artifact commit** for the bundle (never amend). CI `gate` enforces no drift.
- Reflash all units on the bench via twiboot `/reflash-units`.
- v1 `ESPMaster` frozen: it never issues `CMD_READ_DIAG`; unaffected.
- Unit flash headroom on the Nano must be checked after the addition (small
  static firmware; a few hundred bytes expected — verify, don't assume).

## 6. Testing

- **Unit (native):** `unitVccFromAdc` conversion + diag packet build/checksum
  (pure header).
- **Master (native):** `/api` JSON well-formed; **legend-completeness guard**
  (every emitted `/units/health` key ∈ legend); `/system/info` JSON;
  `/status` composition; `/units/health` parse of a `diagV2` packet incl. the
  headline `vccMin` aggregate; graceful `diagV2=false` on a pre-diag reply.
- **Follower (native):** `/api`; `/cluster/health` diag fields; unit diag parse;
  the `fake_leader.py`/`fake_follower.py` twin contract still holds.
- **Bench:** reflash units; verify Vcc reads sane (~4.8–5.1 V idle) and `vmin`
  dips during a full-display move; headline `vccMin` drops on a big move; curl
  every new endpoint on both boards; confirm un-reflashed unit degrades to
  `diagV2=false`.

## 7. Open decisions

None outstanding. (Resolved: stall counter cut; Vcc-min since-boot; legends
cover all terse endpoints.)
