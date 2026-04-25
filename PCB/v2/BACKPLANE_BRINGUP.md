# Backplane PCB v2 — Bring-up procedure

> Per-segment + per-case bring-up sequence. Created 2026-04-25.

## Pre-power inspection

1. Visual: verify all 4 unit-slot 2×3 box-header sockets are populated and oriented correctly (pin 1 silkscreen).
2. Verify 4× 0.2 A polyfuses populated (one per slot).
3. Outer segments: verify J_RJ45 case-level RJ45 is populated (segment-1 has J_RJ45_IN, segment-4 has J_RJ45_OUT).
4. **Chain-end case backplane segment-4 only**: verify R_TERM (120 Ω) is populated. **All other case backplanes**: verify R_TERM is DNP.
5. Verify 4× test pads (V48, GND, A, B) are accessible per segment.

## Step 1 — Per-segment standalone (V48 only, no units)

For each segment in turn:
1. Apply 48 V to the upstream connector (J_SEG_IN or J_RJ45_IN for outer-segment-1).
2. Verify TP_V48: 47.5 V ± 0.5 V at the test pad.
3. Verify each polyfuse output: 47.5 V (within polyfuse R_DCmax × 0 A drop, i.e. essentially identical).
4. Verify TP_GND: 0 V (relative to PSU GND).
5. Verify there is NO 3.3 V or 12 V present (backplane is fully passive).
6. Verify A/B test pads: idle voltage depends on master bias — without master connected, expect floating (high-Z, possibly drifting).

## Step 2 — Inter-segment chain test

Connect 4 segments in a row via 6-pin inter-segment headers:
1. Apply 48 V to segment-1's upstream connector.
2. Verify TP_V48 on each subsequent segment (2, 3, 4): all 47.5 V (V48 traces are pass-through with negligible drop at no load).
3. Verify GND continuity across all 4 segments with a multimeter.
4. Verify A/B continuity: probe TP_A on segment-1, expect electrical continuity to TP_A on segment-4 (single shared net across the case).

## Step 3 — Termination check (chain-end case backplane only)

On the chain-end case backplane (the one with R_TERM populated on segment-4):
1. Disconnect everything. Measure resistance across TP_A and TP_B on segment-4.
2. Expected: 120 Ω ± 1 % (the populated R_TERM).
3. On non-chain-end case backplanes: TP_A↔TP_B should be open (R_TERM DNP, so >> 100 kΩ).

## Step 4 — Polyfuse trip test (optional, destructive on the test cable)

For each slot in turn:
1. With segment energized at 48 V, briefly short the slot's V48 pin to GND through a current-limited bench supply (or an inline 1 Ω test resistor).
2. Verify the polyfuse trips within ~1 s (current → 0 mA, slot V48 drops to ~0 V).
3. Remove short; polyfuse auto-resets within ~30 s.
4. Verify other slots on the same segment remained energized during the fault (rest of bus stays up).

## Step 5 — Full-case bring-up (with master + units)

1. Connect master's bus-1 RJ45 to chain-end case J_RJ45_IN (or first case J_RJ45_IN if multi-case).
2. Populate one unit in slot-1 of segment-1; leave other slots empty.
3. Power up master.
4. Verify master's INA237 reads steady ~25 mA on bus-1 after enable + settle (single populated unit at idle).
5. Verify the unit's STATUS LED enters 2 Hz blink (awaiting address).
6. Master discovers UID; unit transitions to solid LED.
7. Master sends test step command; unit moves the stepper.

Repeat with units in slots 2, 3, 4 — verify each enumerates with a unique address.

## Step 6 — Multi-segment + multi-unit

Populate all 16 slots (4 segments × 4 units each). Verify:
1. Master's INA237 on bus-1 reads ~400 mA at idle (16 × 25 mA).
2. Master's UID discovery enumerates all 16 units within ~10 s cold first-boot, ~100 ms warm.
3. Issue a `BROADCAST_SET_POSITION` command — all 16 units acknowledge within one bus round-trip.

## Common bring-up faults

| Symptom | Likely cause |
|---|---|
| TP_V48 reads 0 V despite 48 V applied | Polyfuse open or input connector mis-mated |
| One slot's V48 reads 0 V, others fine | That slot's polyfuse tripped (check for short on the unit) |
| Master sees 0 mA on bus despite units plugged in | Bus A/B continuity broken — check inter-segment connector mating |
| Unit enumerates but loses bus after a few seconds | Termination missing on chain-end (R_TERM not stuffed when it should be) — reflections eventually corrupt frames |
| All units disappear when one unit's polyfuse trips | Bus shouldn't be affected — if it is, polyfuse is shorting V48 to GND on trip somehow; check polyfuse footprint orientation |
| Heat localized on one polyfuse | That slot's unit is drawing > 200 mA continuously — check stepping behavior |

## Open verification items

1. **Inter-segment connector durability** — JLC PCBA 2×3 box headers are rated for ~50 mate cycles. For a permanent install, this is fine. For repeated bring-up testing, consider locking the inter-segment joint with a small screw bracket from the mech freelancer.
2. **Cable shield bond verification** — confirm the J_RJ45 shield can pads on the backplane are actually NC (not silently bonded to GND through a residual trace). Probe with a multimeter between RJ45 metal can and TP_GND — should be open.
