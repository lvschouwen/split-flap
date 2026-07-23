# Unit self-diagnostics v2 — extended per-unit telemetry (`GET_EXT_DIAG`)

Design for the unit-firmware slice of epic #365. Children: #368 (reset-cause/reboot/uptime),
#369 (unit-side bus diag), #370 (steps-to-home trend), #371 (per-move Vcc sag),
#372 (hall-edge-per-rev), #373 (duty counter), #374 (stall/jam bit).

The master-only slice of #365 (#366 low-Vcc alert, #367 I2C attribution, #375 400 kHz)
already shipped in v2026.07.23. This slice extends per-unit diagnostics that require the
unit to measure and report new signals.

## Premise correction (why this is not one big new packet)

The epic originally framed all seven children as a single new `GET_EXT_DIAG_V1` packet
requiring one reflash. That over-counted the new work: the existing `GET_STATUS` (0x83, #47)
packet **already transmits** most of #368/#369 and coarse #370, and the master **already
decodes and emits** them in `/units/health`:

| Signal | Issue | GET_STATUS wire | Master (`UnitStatus`) | JSON key |
|--------|-------|-----------------|-----------------------|----------|
| reset-cause (MCUSR) | #368 | byte 1 | `mcusrAtBoot` | `mc` |
| brownout count | #368 | byte 2 | `lifetimeBrownoutCount` | `br` |
| watchdog count | #368 | byte 3 | `lifetimeWatchdogCount` | `wd` |
| uptime seconds | #368 | bytes 4–5 (u16 BE) | `uptimeSeconds` | `up` |
| bad-command count | #369 | byte 6 | `badCommandCount` | `bc` |
| last-home step count | #370 | byte 7 (×16, coarse) | `lastHomingStepCount` | `hs` |

So #368 and the honest half of #369 are already data-complete end-to-end; they lack only the
*surfacing* layer (readable decode, transition logging, HA). That is master-only work — no
reflash. Only the genuinely-new signals (fine #370, #371, #372, #373, #374) need new unit
firmware.

## Shape: one combined arc, one reflash

Single branch/PR. Two internal slices, sequenced A before B:

- **Slice A — master-only (no reflash).** Surface the already-transmitted #368/#369/coarse-#370
  data. OTA-benchable immediately.
- **Slice B — `GET_EXT_DIAG` (one reflash).** A lean new packet for the new-measurement signals,
  flashed to the fleet over I2C/twiboot at bench-verify.

## Packet: `GET_EXT_DIAG` — opcode `0x89`, 11-byte reply

Unversioned opcode, matching the five existing query packets (`GET_VITALS`/`ODOMETER`/`DIAG`/
`SELF_TEST`/`STATUS`). No wire version byte: backward-compat is the established masked-XOR
checksum degrade (an un-flashed unit answers the unknown opcode with garbage/0xFF; the checksum
rejects it and the master sets `diagExt=false`), and a future format change takes a new opcode
(`0x8A`) exactly as `GET_VITALS` (0x88) was added. The opcode map in `SplitFlapProtocol.h` is the
real versioning mechanism. (See #378 for the codebase-wide version-naming audit this decision spun off.)

New pure header `UnitExtDiag.h`, copied verbatim into `firmware/v2/Master` and
`firmware/v2/FollowerEsp01` (copy policy — fix bugs in all trees; drift-gated by
`tests/test_copied_headers.py`).

| off | field | type | source (unit-side) | issue |
|----:|-------|------|--------------------|-------|
| 0–1 | `stepExcessLast` | u16 LE | last home: `actual − geometry-expected` steps | #370 |
| 2–3 | `stepExcessMax`  | u16 LE | worst-seen excess since boot (drag alarm) | #370 |
| 4–5 | `vccSagLastMove` | u16 LE mV | min Vcc during last move (reset at move start) | #371 |
| 6   | `hallEdgesLastRev` | u8 | entering-edges in last completed rev (1=OK, 0=miss, ≥2=noise) | #372 |
| 7–8 | `dutyWindow` | u16 LE | moves in a rolling ~60 s window | #373 |
| 9   | `statusBits` | u8 | bit0 last-move stall/jam, bits1–7 reserved | #374 |
| 10  | `checksum` | u8 | XOR of bytes 0–9 ^ `EXT_DIAG_CHECKSUM_MASK` | — |

11 bytes, well under the AVR 32-byte TWI buffer.

### Why step *excess*, not raw steps-to-home (#370)

Raw steps-to-home is dominated by wherever the drum happened to stop before a home (0..STEPS,
effectively random), so a raw trend is noise. The mechanical-drag signal is *lost steps*:
`actual − expected`, where expected is the step count the geometry predicts from the believed
`drumPosition` to the hall edge. Excess is start-position-independent and directly measures
slip/drag. We report `stepExcessLast` (current) + `stepExcessMax` (worst-seen = the alarm).
The coarse `hs` in `GET_STATUS` stays as-is for backward context.

### Why #369 ships as the counter only

Codex's epic verdict already narrowed #369 against dishonest timing claims. A TWI-ISR
"handler latency µs" is not honestly measurable, so it is dropped. `badCommandCount` is the
solid, real half — and it is already transmitted (`GET_STATUS` byte 6) and in JSON (`bc`).
#369 therefore has no new unit-side field in `GET_EXT_DIAG`; Slice A just surfaces the counter
it already has.

## Poll cadence

`GET_EXT_DIAG` is polled round-robin, one unit per heartbeat tick, so the whole wall is covered
every ~N ticks (16 units ≈ once/16 s at the 1 Hz heartbeat). This spreads the extra I2C
transaction flat — never a burst — respecting the #326 lesson that bus time, not flash, is the
scarce resource. Diagnostics are slow-moving, so per-unit freshness on the order of tens of
seconds is fine.

## Unit-side implementation (Slice B)

All new signals hang off existing hooks; no new EEPROM slots.

- **#370 excess** — `calibrate()` already step-counts to the hall edge via `stepCounted`. Expected
  steps = distance from believed `drumPosition` to the edge in the rotation direction; excess =
  `actual − expected` (clamped ≥ 0). Track last + saturating max.
- **#371 per-move sag** — vitals already samples Vcc mid-move for the since-boot min. Add a
  per-move latch: reset at `rotateToLetter` entry, sample during, hold at move end.
- **#372 hall edges/rev** — `driftObserveEdge` already detects entering edges; count them between
  odometer revolution boundaries (`stepAccumulator` wrap) and latch the last completed rev's count.
- **#373 duty window** — count moves in a rolling ~60 s window (decaying counter / coarse ring).
- **#374 stall bit** — `rotateToLetter` knows commanded steps × speed → expected ms; if actual
  move duration exceeds expected × margin, set `statusBits` bit0. Hall anomaly is not a bit — the
  master derives it from the raw `hallEdgesLastRev` count (single source of truth).

Pure logic (encode/expected-steps/degrade) in `UnitExtDiag.h`, natively tested. AVR ADC/hall/
timing glue lives in the `.ino` files (bench tier). Deferred-work discipline unchanged — nothing
new runs in the Wire ISR.

## Master-side surfacing

### Slice A — from existing `GET_STATUS` fields (no reflash)

- **Readable reset-cause**: decode the raw `mc` MCUSR bitfield → `power-on` / `brownout` /
  `watchdog` / `external`. Pure helper, natively tested (extend `UnitEventLog.h`).
- **Transition log lines** (the #322 pattern): reboot detected (mcusr/uptime-reset edge),
  reboot-count climb. Emitted per-unit in `heartbeatTick`, where each signal is coherent — same
  seam #322 uses. Onset/recovery edge logic pure and tested.
- **HA**: reset-cause + reboot-count (brownout + watchdog) sensors.

### Slice B — from `GET_EXT_DIAG` (needs reflash)

- New `/units/health` JSON keys behind a `diagExt` valid flag (`se`, `sx`, `sag`, `he`, `dw`,
  `sb`); absent on un-flashed units, exactly like the vitals `diagV2` degrade.
- **Log edges**: jam onset (`statusBits` bit0), steps-excess over threshold (drag), hall anomaly
  (`hallEdgesLastRev` ≠ 1).
- **HA**: jam `binary_sensor`; steps-excess sensor.

## Testing

- **Native (RED-first, TDD):** `UnitExtDiag.h` encode/decode + checksum + all-0x00/0xFF/repeated
  degrade; expected-steps / excess math; reset-cause decode; every new event-log edge
  (onset + recovery where applicable). Register in the native runner.
- **Gates:** bump `UNIT_HEALTH_JSON_CAP` headroom tests for the new keys; update the copied-header
  manifest (`tests/test_copied_headers.py`) and the ESP-01 api-index gate (`tests/test_api_index.py`)
  in the same commit that adds `UnitExtDiag.h` / routes, so drift gates stay green.
- **Bench (E2E tier):** verify readable reset-cause + reboot logging on a real unit power-cycle;
  jam bit by physically stalling a flap; excess trend by observing a healthy vs. dragging unit;
  hall-edges = 1 on a healthy rev.

## Deployment

- **Bundle flow (load-bearing):** rebuild Unit clean → `make_manifest.py stage` (writes the hex
  into Master *and* FollowerEsp01 `data/`) → rebuild Master + FollowerEsp01 → **separate** artifact
  commit (never amend the bundle into a code commit).
- **Rollout:** OTA the leader S3 first (an older-build leader downgrades followers via the cluster
  rollout), then each master; trigger `/reflash-units` per master to flash its Nano row over
  I2C/twiboot.
- **Pre-flight caveat:** twiboot listens on the DIP-derived address, so over-I2C reflash only
  reaches a unit whose EEPROM address == DIP (or no EEPROM address burned). Confirm the wall units
  are DIP-addressed before flashing; any non-DIP unit needs a manual USB touch.

## Out of scope

- Adaptive/self-tuning stagger from #371 sag data (#324 twin) — report-only first, per the epic split.
- Any wire version byte on `GET_EXT_DIAG` (see #378).
- `GET_STATUS` layout changes — it already carries #368/#369/coarse-#370; left untouched.
