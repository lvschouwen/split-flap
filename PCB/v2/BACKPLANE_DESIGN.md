# Backplane PCB v2 — Schematic-level design

> Complement to `BACKPLANE_DECISIONS.md`. Schematic-level detail for the layout designer. Created 2026-04-25 (#76 backplane pivot).
>
> Backplane is **passive** (no active silicon). This doc is short.

## Per-segment block diagram (one of 4 identical)

```
Inter-segment IN connector (6-pin 2.54mm): V48|A|V48|B|GND|GND
  │
  ├──── V48 trace (TOP layer, 2 mm wide, 1 oz Cu)
  │       │
  │       ├── 0.2A polyfuse ── slot-1 V48 pin (and pin 3 paralleled)
  │       ├── 0.2A polyfuse ── slot-2 V48 pin
  │       ├── 0.2A polyfuse ── slot-3 V48 pin
  │       ├── 0.2A polyfuse ── slot-4 V48 pin
  │       │
  │       └── pass-through to inter-segment OUT connector
  │
  ├──── GND trace (BOT layer, 2 mm wide, 1 oz Cu) — paralleled GND pins
  │
  └──── A/B differential pair (TOP layer, 120Ω diff impedance over BOT GND)
        │     ↓ tap to each slot's pins 2, 4
        ↓
        Inter-segment OUT connector (6-pin)

Per slot (×4 per segment):
  Slot N socket (2×3 box header female, 2.54mm)
    pin 1 ── V48 (after polyfuse)
    pin 2 ── A (bus tap)
    pin 3 ── V48 (paralleled with pin 1, same polyfuse output)
    pin 4 ── B (bus tap)
    pin 5 ── GND
    pin 6 ── GND

Test points (one set per segment, edge-accessible):
  TP_V48, TP_GND, TP_A, TP_B (1 mm SMD pads)

Outer-segment-only:
  Case-level RJ45 (shielded THT) on the outer end of segment-1 (chain-IN)
  and segment-4 (chain-OUT). Pinout per BACKPLANE_DECISIONS §"Case-level
  RJ45". RJ45 shield pads DNP (no shield bond on backplane).

End-of-chain-segment-only (segment-4 of chain-end case, PCBA stuffing variant):
  R_TERM 120Ω across A/B near the case-level RJ45 (chain electrical end).
```

## Component selection

| Ref | Function | Part | Footprint | Notes |
|---|---|---|---|---|
| F1..F4 | Per-slot V48 polyfuse 0.2A hold / 0.4A trip | nanoSMDC020F | 1812 SMD | One per unit-slot; auto-reset; trip on stuck stepper coil |
| J_SLOT1..4 | Unit-slot socket | 2×3 2.54 mm box header female | 2×3 SMD or THT | Polarised shroud preferred |
| J_SEG_IN, J_SEG_OUT | Inter-segment connector | 2×3 2.54 mm pin header | Same as J_SLOT (mirrored polarity) | OR all-female + jumper plug — TBD at capture (B1) |
| J_RJ45_IN | Case-level RJ45 chain IN | Shielded THT 8P8C no-mag | THT shielded | Outer-segment-1 only |
| J_RJ45_OUT | Case-level RJ45 chain OUT | Shielded THT 8P8C no-mag | THT shielded | Outer-segment-4 only |
| R_TERM | RS-485 termination 120Ω 1% | 0805 | 0805 | DNP except chain-end case backplane (PCBA stuffing variant) |
| C_BULK | V48 bulk smoothing 22µF/100V X7R | 1210 | 1210 | One per segment near inter-segment IN connector |
| C_BYP | V48 HF decap 100nF/100V X7R | 0603 | 0603 | One per slot near polyfuse output |
| TP_V48, TP_GND, TP_A, TP_B | Test pads 1 mm SMD | — | 1 mm SMD pad | Per segment, edge-accessible |

## Differential routing

- A/B pair routed as **120 Ω differential impedance over a solid GND reference plane**.
- Trace width / gap calculated against JLC's 2-layer 1.6 mm stack (typical: ~0.35 mm trace / 0.4 mm gap; layout designer confirms).
- **Continuous solid GND on bottom layer** under the A/B pair across the whole 320 mm segment.
- Each slot is a stub tap; stub length from main A/B trace to socket pin ≤ 5 mm.
- Inter-segment connector imposes a controlled discontinuity — keep its pad geometry tight to maintain impedance.

## V48 power distribution

- 2 mm trace width on TOP layer (1 oz Cu) for V48 main bus across segment.
- Per IPC-2221: 1 oz × 2 mm trace at 10 °C rise carries ~3 A — comfortably above the segment's ~0.4 A worst-case (4 slots × ~100 mA).
- GND return: BOT-layer plane — solid pour, also 2 mm minimum width where it bridges through inter-segment connectors.

## Open issues

- **B1** — Inter-segment connector mating direction (M-F vs F+jumper). Capture-time decision.
- **B5** — Slot connector vendor (generic 2×3 box header from JLC Basic library vs Wurth/Molex named parts). Cost difference ~€0.05/slot. Defer to BOM-finalize.
- **B6** — Whether to add LEDs per slot for "slot powered" / "slot data" indication. Saves debug time at scale; adds ~€0.30/slot. Defer to v2.1.
