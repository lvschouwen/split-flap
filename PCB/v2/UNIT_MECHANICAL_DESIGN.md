# Unit PCB v2 — Mechanical design

Complement to `UNIT_DECISIONS.md` §Mechanical decisions. Placement brief, AS5600 integration, and mounting sketch.

## Board specification

- **Dimensions target: 65 × 35 mm** (within 86 × 40 mm hard max).
- **Layer stack**: 2-layer, 1.6 mm FR-4, 1 oz / 1 oz copper, HASL finish (ENIG only if signal-integrity review flags the RS-485 differential routing).
- **Corner treatment**: 1 mm outside radius — pick-and-place friendly, avoids sharp corners snagging bracket walls.
- **Silkscreen**: white on top; minimal bottom (reference designators only if needed).

## Floorplan (sketch — not final layout)

```
 +-------------------------------------------------------------+
 |                                                             |
 | [RJ45 IN]                [AS5600 + magnet keepout 10×10]   |
 |                          [STM32G030]                       |
 |                          [SN65HVD75] [ULN2003A]            |
 | [TVS + ESD]                                                 |
 |                          [Buck + L + C]                    |
 |                          [LDO]                              |
 |                                        [120Ω + JMP hdr 1×2]|
 |                                        [JST-XH 5-pin]      |
 | [RJ45 OUT]                                                  |
 |                                                             |
 +-------------------------------------------------------------+
   ← 65 mm →
    ↑
   35 mm
    ↓
```

## Key placement rules

1. **RJ45 IN and OUT on opposite short edges.** Cables exit opposing directions → linear daisy-chain routing behind a wall of flap units. No cable-routing gymnastics at assembly time.
2. **AS5600 IC centered on the "motor axis" reference point.** This point is mechanically defined by the 28BYJ-48 output shaft position on the final assembly. Every other component clusters around this anchor.
3. **Magnet keepout: 10 × 10 mm copper-free** zone on both layers around the AS5600 IC body. No ferrous material (screws, shielded RJ45 cans, inductor cores) within 5 mm radial of the IC. RJ45 shield cans are the closest potential offender — verify clearance during layout.
4. **ULN2003 adjacent to JST-XH motor connector.** Short, direct traces to the stepper coils keep noise off the rest of the board.
5. **Buck IC + inductor tightly grouped**, input cap within 2 mm of VIN, output cap between inductor and VOUT sense. Switch-node loop ≤ 5 mm.
6. **TVS near RJ45 power pins.** One shared TVS at the midpoint between IN and OUT power pins is acceptable given the low-impedance 48V plane between jacks.
7. **ESD array ≤ 5 mm from RJ45 data/wake pin entry**, before trace reaches SN65HVD75 or MCU.
8. **120 Ω terminator + 2-pin 2.54 mm jumper header accessible from top side** after enclosure assembly. Place near the OUT RJ45 edge so the installer can reach it to fit/remove the shunt without disassembling the bracket. Header vertical orientation; shunt clearance ≥ 10 mm above board.

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
- Hole positions coordinated with 3D-printed bracket design (separate mechanical artifact).
- Alternatively: corner slots/tabs for a snap-in plastic clip — decide when bracket is designed.

## Motor cable entry

- JST-XH 5-pin vertical connector on the side of the PCB facing the motor body.
- Cable exits parallel to motor axis — length chosen for drum clearance without stress.
- Alternative right-angle variant available if a flat cable-exit is required by the bracket (same part family, different mount angle).

## Thermal considerations

| Source | Dissipation | θ_JA | Rise (ambient 40 °C) |
|---|---|---|---|
| ULN2003A SOP-16 | ~300 mW (2-phase, 74 mA each, 2 V sat) | ~100 °C/W | ~30 °C |
| HT7833 LDO SOT-89 | ~435 mW (worst-case logic) | ~150 °C/W | ~65 °C |
| TPS54308DBV | ~530 mW (85 % eff at 250 mA avg) | ~200 °C/W with thermal vias | ~100 °C |

**All parts within 125 °C T_J limit at 40 °C ambient.** The buck is the tightest margin — confirm thermal vias (4–8 under the IC + nearby GND pour) during layout review.

No heatsinks needed. Natural convection sufficient at the projected duty cycle (flap units are idle most of the time).

## Enclosure assumptions

- 3D-printed plastic bracket carries the PCB behind the 28BYJ-48 motor.
- Plastic = no electromagnetic shielding, no ferrous interference with AS5600.
- LED visible through a small aperture on the enclosure (~2 mm) or via a light pipe. Placement of LED in floorplan should consider visibility.
- RJ45 connectors may or may not be flush with enclosure openings — depends on bracket design. Floor-plan assumes opposing-edge placement gives bracket-designer flexibility.

## Open issues (non-blocking)

- **M1** — Final mounting-hole positions pending 3D bracket design.
- **M2** — Exact AS5600-to-RJ45-shield clearance verified during routing — may push RJ45s further from the motor axis center.
- **M3** — Buck thermal pad sizing + thermal via count pending layout review.
- **M4** — Enclosure + flap-drum access + cable management — all mechanical-handoff-level, out of PCB scope for this doc.
- **M5** — LED visibility: whether a light-pipe or just a through-aperture is needed depends on bracket thickness; pass to the mechanical freelancer.
- **M6** — Whether to break-off tabs or route around for depanelization in the 10-unit prototype run — decide with JLC ordering.
- **M7** — Termination shunt vertical clearance inside the enclosure: bracket designer must leave ≥ 10 mm above the 2-pin jumper header so the shunt fits and remains removable.
