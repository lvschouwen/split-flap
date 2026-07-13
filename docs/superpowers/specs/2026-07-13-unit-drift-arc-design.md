# Unit drift arc — drift detection + auto re-home (#263), physical-letter readback (#264), self-test op (#265)

Date: 2026-07-13
Issues: #263, #264, #265
Scope: `firmware/v1/Unit` (first unit change since #231), `firmware/v1/shared/SplitFlapProtocol.h`, `firmware/v2/Master` (+ web `data/`). v1 ESPMaster untouched (#217 retirement).

## Problem

Missed steps make a unit drift off-by-N flaps until the next backward move triggers `calibrate(false)`. Between calibrations the drift is invisible: the unit's `displayedLetter` is a belief, `GET_LETTER` reports that belief, and the master's post-frame verify confirms the belief against itself. There is no measurement of the drum's *physical* position and no self-correction while the display sits on forward-only sequences (clock mode).

Key geometric fact that shapes the design: the drum only rotates forward and letter 0 sits at the hall marker, so a forward move from letter i to j>i never crosses the marker — *unless the drum has physically slipped*. An unexpected hall edge during a forward move is therefore a direct drift observation, and it is the only drift signal available before the next natural calibrate.

## Decisions (user-approved 2026-07-13)

- Arc = all three issues, one branch/PR, one unit-firmware bundle + bench cycle.
- Auto re-home policy: at idle — 2 s command-quiet, then `calibrate(true)` + return to commanded letter. 60 s cooldown between auto re-homes (a broken unit must not spend its life homing; `driftEvents` keeps counting).
- Master mismatch policy: **surface only** (health JSON + web badges). No corrective resend, no HA entity — the unit is the single correction authority.
- Approach A: tracked drum position + per-step hall watch (not calibrate-only measurement).
- `driftEvents` is since-boot RAM (saturating uint8). No new EEPROM slot, no layout migration.
- Drift threshold 23 steps (~½ flap); hall edge debounce = 2 consecutive samples.
- Self-test runs at `HOMING_RPM`.

## Unit firmware

### `UnitDrift.h` — pure, natively tested

`DriftState`: `drumPosition` (uint16, steps past the hall edge, mod `STEPS`), `positionKnown`, `driftEvents` (saturating uint8), `lastDriftSteps` (int16), `driftPending`.

Pure functions:
- `driftAddSteps(state, steps, stepsPerRev)` — advance position mod rev.
- `driftFoldDeviation(pos, stepsPerRev)` — fold to signed [−rev/2, rev/2).
- `driftOnHallEdge(state, thresholdSteps)` — deviation vs expected edge (position ≡ 0), event decision, resync position to 0; returns the folded deviation.
- `driftExpectedCalibrateSteps(displayedLetter, calOffset, ...)` — expected step count for `calibrate()`'s marker search from the current belief (including the 50-step move-out).
- `driftPhysicalLetter(state, calOffset, stepsPerFlap...)` — 0..44 estimate, 0xFF when `!positionKnown`.
- `driftEncodeDiagReply(...)` / threshold + wire constants.

### Integration

- `stepCounted()` (the #231 single motion funnel) also advances `drumPosition`.
- `stepFlaps()` converts from per-flap chunks to per-step stepping (`Stepper::step(1)` keeps inter-step pacing via its `last_step_time` member; the added `digitalRead` ≈ 4 µs vs ≥ 2 ms/step budget). Each step samples the hall with 2-sample debounce; a 1→0 edge → `driftOnHallEdge`: past threshold → `driftEvents++`, `lastDriftSteps`, `driftPending = true`. Position resyncs to ground truth either way; `displayedLetter` (the belief) is NOT touched — the re-home restores truth.
- `calibrate()`: on marker found, if `positionKnown` and not the boot calibration, compare measured `i` against `driftExpectedCalibrateSteps()`; past threshold → `driftEvents++` + `lastDriftSteps`. Always: resync `drumPosition = 0` (the subsequent `calOffset` move flows through `stepCounted`), set `positionKnown`, clear `driftPending`.
- `loop()` idle re-home drain (checked before the sleep gate): `driftPending && !currentlyrotating && displayedLetter == receivedNumber && (2 s since last command activity) && (60 s since last auto re-home)` → `calibrate(true)`, clear pending, stamp cooldown; the existing letter-diff check then rotates back to `receivedNumber`. The overheat gate is already satisfied by the 2 s wait. `previousMillis` is refreshed so sleep doesn't race the return move.
- ISR mirrors (house pattern — volatile, multi-byte updates under `noInterrupts()`): physical-letter estimate, flags, `driftEvents`, clamped `lastDriftSteps`. `SFP_CMD_GET_DIAG` sets `pendingDiagResponse`; `requestEvent()` replies 6 bytes.

### Self-test (#265)

`SFP_CMD_START_SELF_TEST` → `pendingSelfTest` flag → drained in `loop()` like every mutation. Procedure (all at `HOMING_RPM`, `currentlyrotating = 1`, WDT kicked per step):
1. Sync: find hall 1→0 edge (calibrate-style, including the step-out-of-window rule).
2. Measured revolution, stepping 1 at a time: steps until the next 1→0 edge = **actual steps/rev**; samples with hall == 0 = **hall window width**; `millis()` delta = **rev time**.
3. Timeout 3×`STEPS` in either phase → state = failed.
4. End: at edge → step `calOffset`, `displayedLetter = 0`, drift state fully resynced; loop restores the commanded letter.

Result mirror: state (0 never / 1 running / 2 ok / 3 failed) + the three u16 measurements. `SFP_CMD_GET_SELF_TEST` replies 9 bytes. Duration ≈ 2 revolutions ≈ 12 s.

## Wire protocol (`shared/SplitFlapProtocol.h`, additive)

| Opcode | Value | Reply |
|---|---|---|
| `SFP_CMD_GET_DIAG` | 0x86 | 6 B: phys letter (0xFF unknown), flags (bit0 driftPending, bit1 positionKnown), driftEvents, lastDriftSteps (int8, clamped ±127), reserved 0, XOR checksum ^ 0xB7 |
| `SFP_CMD_GET_SELF_TEST` | 0x87 | 9 B: state, stepsPerRev u16 LE, hallWindowSteps u16 LE, revTimeMs u16 LE, reserved 0, XOR checksum ^ 0x5C |
| `SFP_CMD_START_SELF_TEST` | 0x98 | none (mutation band) |

`GET_LETTER` (0x84) keeps its exact meaning — the belief. `GET_DIAG` reports the hall-corrected physical estimate; that distinction is what makes #264 additive rather than a duplicate. Old units bump `badCommandCount` on unknown opcodes; v1 masters never send these.

## v2 master

- `UnitProtocolHelpers.h` (v2 copy): `decodeDiagReply`, `decodeSelfTestReply` — pure, natively tested. (v1 tree gets no decoder — nothing there calls it.)
- `UnitBus`: `unitBusReadDiag(addr, UnitDiag&)` (folded into `unitBusPollHealth`), `unitBusStartSelfTest(addr)`, `unitBusReadSelfTest(addr, ...)`. Sketch-mode ops — no twiboot interaction, probe-inhibit rules unaffected.
- Self-test = new `DisplayCommand` op riding the `{"seq":N}` → `GET /unit/op-result` contract; validation in `MaintenancePolicy.h` (unit index in probed width, unit in sketch mode); displayTask executes: start, poll status/result until done or 30 s timeout; op-result JSON `{state, steps_per_rev, hall_window, rev_time_ms}`.
- `UnitFacts` + snapshot: diag fields + `diagValid`; displayTask records the last frame's intended letters so `/units/health` can emit per-unit `phys`, `drift`, `mismatch`.

## Web UI (v2 `data/`, selectors preserved)

- Health table: **Drift** column (driftEvents), mismatch badge on the unit row.
- Flap mirror: subtle per-cell mismatch marker + tooltip (intended vs physical). No layout change.
- Maintenance tab: per-unit **Self-test** button + inline result (steps/rev vs nominal 2038, hall window, time/rev), existing per-unit-op pattern (button lockout while op pending).

## Testing

- Native, Unit dir: `UnitDrift.h` (mod arithmetic, fold, threshold, expected-calibrate-steps, letter estimate, encode), self-test encode.
- Native, v2 Master: decoders (incl. checksum-reject), `MaintenancePolicy` self-test validation, health JSON shape.
- Bench (E2E tier): see rollout checklist in the PR — drift induced by pinching a drum mid-move; self-test plausibility per unit; new bundle reflash doubles as the parked #250 batch-4 brownout gate.

## Rollout

Bundle flow: commit code → clean unit build → `make_manifest.py stage` (writes BOTH master trees) → separate artifact commit → v2 master OTA → `/reflash-units`. One branch (`unit-drift-arc`), one PR closing #263/#264/#265.

## Risks / watch items

- Per-step loop timing at max message speed — bench-verify no audible cadence change.
- Hall edge repeatability vs the 23-step threshold — if bench shows false positives, threshold is a one-line constant.
- A pathological unit (loose magnet) re-homing every 60 s: acceptable duty; visible via driftEvents + #265 baseline.
- Unit flash headroom on the ATmega328P — check `pio run` size delta; ~1–1.5 KB expected.
