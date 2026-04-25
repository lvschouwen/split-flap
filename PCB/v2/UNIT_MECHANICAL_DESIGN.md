# Unit PCB v2 — Mechanical design

Complement to `UNIT_DECISIONS.md` §Mechanical decisions. Placement brief, AS5600 integration, and mounting sketch. Last edited 2026-04-25 (#76 backplane pivot).

## Board specification

- **Dimensions target: 75 × 35 mm** (revised 2026-04-25 from 65 × 35 mm — accommodates LQFP-32 STM32G030 + HSOP-8 PowerPAD TPS54360 + AS5600 ferrous keep-out + motor connector + backplane connector). 80 mm pitch case allows a small mechanical clearance around the unit board.
- **Layer stack**: 2-layer, 1.6 mm FR-4, 1 oz / 1 oz copper, HASL finish (ENIG only if signal-integrity review flags the RS-485 differential routing — unlikely at 500 kbaud over an internal stub).
- **Corner treatment**: 1 mm outside radius — pick-and-place friendly, avoids sharp corners snagging bracket walls.
- **Silkscreen**: white on top; minimal bottom (reference designators only if needed).

## Floorplan (sketch — not final layout)

```
 +-------------------------------------------------------------+
 |                  [AS5600 + magnet keepout 10×10]            |
 |                  [STM32G030 LQFP-32]                        |
 |  [TPL7407L]                  [SN65HVD75]                    |
 | [Buck + L + C]                                              |
 | [LDO]    [TVS + SM712 ESD]                                  |
 | [JST-XH 5-pin]                  [Backplane 2×3 box header]  |
 | [SWD pogo pads (4×)]                                        |
 +-------------------------------------------------------------+
   ← 75 mm →
    ↑
   35 mm
    ↓
```

## Key placement rules

1. **Backplane connector on the back-edge of the unit PCB.** Single 2×3 box header (2.54 mm pitch) carries V48/V48/GND/GND/A/B from backplane to unit. Unit slides perpendicular into backplane — no cables. Replaces the pre-2026-04-25 IN/OUT RJ45 scheme.
2. **AS5600 IC origin (0,0) reference point.** Mechanically defined by the 28BYJ-48 output shaft position on the final assembly. **The mechanical bracket is designed by the mech freelancer**, who hands the PCB designer a STEP file showing AS5600-to-shaft alignment. PCB designer hands back a STEP showing AS5600 IC center coordinate + mounting hole positions.
3. **Magnet keepout: 10 × 10 mm copper-free** zone on both layers around the AS5600 IC body. No ferrous material (screws, inductor cores, motor body) within 5 mm radial of the IC. With the backplane pivot, RJ45 shield cans are no longer on the unit — eliminates the closest pre-2026-04-25 ferrous-clearance concern.
4. **TPL7407L adjacent to JST-XH motor connector.** Short, direct traces to the stepper coils keep noise off the rest of the board.
5. **TPS54360DDA buck + inductor tightly grouped**, input cap within 2 mm of VIN, output cap between inductor and VOUT sense. Switch-node loop ≤ 5 mm. **Thermal pad on PowerPAD must connect to ≥ 6 vias into a ≥ 200 mm² bottom GND pour and ≥ 100 mm² top GND pour** (locked spec — TPS54360 in HSOP-8 PowerPAD comfortably hits ≤ 40 °C/W vs the 80 °C/W spec, unlike pre-2026-04-25 TPS54308 which had no thermal pad at all).
6. **TVS (SMAJ51A) near backplane connector +48 V pin entry.** Within 10 mm.
7. **ESD array (SM712) ≤ 5 mm from backplane connector A/B pin entry**, before trace reaches SN65HVD75.
8. **No termination on the unit** (was 120 Ω + jumper pre-2026-04-25). Termination lives on the backplane chain-end segment (PCBA stuffing variant).

## AS5600 integration

| Spec | Value |
|---|---|
| Magnet | 6 mm × 2.5 mm diametral cylinder, neodymium N35 or N42 |
| Position | Glued to a stub shaft extension on the 28BYJ-48 output shaft back-side |
| Airgap | 0.5 – 3 mm between magnet face and AS5600 IC face (target 1.5 mm) |
| Orientation | N-S axis perpendicular to rotation axis (diametral, not axial) |
| Ferrous exclusion | No ferrous material within 5 mm of IC |
| Calibration | First-boot stores "home angle" — the AS5600 reading when flap #0 is in home position |

The AS5600 reads absolute angle [0, 4095] per rotation, which means:
- **No magnetic sweep needed at boot** — position is known instantly.
- **Missed-step detection is continuous** — firmware compares commanded angle vs measured angle on every tick.
- **Recovery from power loss** — position is intrinsically re-established without mechanical homing.

The 28BYJ-48 output shaft is geared 64:1, so its rotation over the AS5600 corresponds to drum rotation directly. For a 45-flap drum, 1 flap = 360 / 45 = 8° of output-shaft rotation = 91 AS5600 counts. Sufficient angular resolution to distinguish every flap position with ~45× headroom.

## Mounting

- **2× M3 isolated through-holes**, diagonally placed near opposite corners.
- No copper connection to mounting holes (prevents ground-loop issues if bracket is conductive in a later revision).
- **Hole positions handed off via STEP exchange with mech freelancer** — they design the bracket-to-PCB interface and the bracket-to-MiddleFrame interface. PCB designer is downstream of bracket geometry. Default during PCB layout: holes on opposite short edges, midline-aligned, ≥ 5 mm from board edge.

## Motor cable entry

- JST-XH 5-pin vertical connector on the side of the PCB facing the motor body.
- Cable exits parallel to motor axis — length chosen for drum clearance without stress.
- Alternative right-angle variant available if a flat cable-exit is required by the bracket (same part family, different mount angle).

## Thermal considerations

| Source | Dissipation | θ_JA (locked) | Rise (ambient 40 °C) | T_J |
|---|---|---|---|---|
| TPL7407L SOIC-16 | ~50 mW (MOSFET R_DS(on) ~0.1 Ω × 300² mA) | 95 °C/W | 5 °C | 45 °C |
| HT7833 LDO SOT-89 | ~435 mW (worst-case logic) | 150 °C/W (with ≥80 mm² pad pour) | 65 °C | 105 °C |
| TPS54360DDA | ~440 mW (~88 % eff at 300 mA avg) | **≤ 40 °C/W (achievable on HSOP-8 PowerPAD with proper pour)** | 18 °C | **58 °C — comfortable margin** |

**Buck thermal spec — LOCKED 2026-04-25 with TPS54360DDA:**

- Minimum **6× thermal vias** (0.3 mm drill, 0.6 mm pad) directly under the TPS54360DDA's HSOP-8 PowerPAD thermal pad, dropped to a continuous ground pour on the bottom layer.
- Bottom-layer GND pour ≥ **200 mm²** contiguous around the thermal-via cluster.
- Top-layer GND pour around the buck IC ≥ **100 mm²**, connected to the bottom pour through ≥ 4 stitching vias at the perimeter.
- Both pours must reach all the way to the unit's mounting holes (additional thermal sink into the bracket if metallic).
- Resulting θ_JA target: **≤ 40 °C/W** (HSOP-8 PowerPAD with proper pour comfortably hits 30–40 °C/W). T_J at 300 mA avg + 40 °C ambient ≈ 58 °C. Massive headroom vs 125 °C abs-max — handles 60 °C ambient with no concern.

**Pre-2026-04-25 design specced TPS54308DBV (SOT-23-6) at "≤ 80 °C/W"** — that target was physically unachievable since SOT-23-6 has no exposed thermal pad (real θ_JA ≈ 165 °C/W → T_J ≈ 140 °C, exceeding abs-max). #76 audit caught the error; TPS54360DDA replacement resolves it.

No heatsinks needed. Natural convection sufficient at the projected duty cycle (flap units are idle most of the time).

## Enclosure assumptions

- 3D-printed plastic bracket carries the PCB behind the 28BYJ-48 motor.
- Plastic = no electromagnetic shielding, no ferrous interference with AS5600.
- LED visible through a small aperture on the enclosure (~2 mm) or via a light pipe. Placement of LED in floorplan should consider visibility.
- RJ45 connectors may or may not be flush with enclosure openings — depends on bracket design. Floor-plan assumes opposing-edge placement gives bracket-designer flexibility.

## Open issues (non-blocking)

- **M1** — Final mounting-hole positions pending 3D bracket design (mech freelancer hand-off via STEP).
- ~~**M2**~~ **Closed 2026-04-25.** With backplane pivot, the unit no longer carries RJ45s — the AS5600-to-RJ45-shield-can clearance concern is eliminated.
- ~~**M3**~~ **Closed 2026-04-25.** Buck swapped to TPS54360DDA (HSOP-8 PowerPAD). θ_JA ≤ 40 °C/W target with standard pour spec, vs the unachievable ≤ 80 °C/W on TPS54308DBV SOT-23-6.
- **M4** — Enclosure + flap-drum access + cable management — mech freelancer scope, out of PCB doc.
- ~~**M5**~~ **Closed 2026-04-25.** LED placement: handed off to mech freelancer with bracket STEP. PCB places LED on the back-edge of the unit so any case-back aperture can see it.
- **M6** — Panelization for 10-unit prototype: defer to JLC ordering. Break-off tabs preferred for hand-rework.
- ~~**M7**~~ **Closed 2026-04-25.** Termination moved to backplane; no shunt clearance needed on unit.
