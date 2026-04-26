# DIN-Rail Bus PCB

**Revision:** 2026-04-26

Replaces the per-row cable harness with a passive bus PCB mounted in a
35 mm DIN rail. Each unit clips onto the rail; pogo pins on the unit's
underside contact the bus PCB's traces. No connectors on the
unit-to-bus link.

**One bus PCB design**, used 8 times in a 4-row system: each row uses
2× 300 mm bus PCBs daisy-chained.

## Per-row topology

```
   Master row port (4-pin: 12V/GND/A/B)
        |
        | master cable (4-conductor, ~30 cm)
        v
   +========================+    +========================+
   | Bus PCB section A      |----| Bus PCB section B      |
   | 300 mm, 8 unit slots   |  ^ | 300 mm, 8 unit slots   |
   |                        |  | |                        |
   | [in] ... [out]         |  | | [in] ... [term out]    |
   +========================+  | +========================+
        ^                      |          ^
        |                      |          |
        units 0..7 clip on    daisy-     units 8..15 clip on
        via DIN rail          chain      via DIN rail
                              cable
                              (4-cond,
                              short)
                                          ^
                                          |
                                  120R termination plug
                                  on the second board's
                                  far-end connector
```

Each row uses 2 bus PCBs because:
- 16 units × ~37 mm pitch = ~600 mm row span.
- JLC standard fab limit is 400 mm; a single 600 mm board pushes into
  premium-fab territory.
- 2× 300 mm boards split through this comfortably and mean one PCB
  design covers the whole 4-row system.

## Trace layout (top view)

The bus PCB has 4 parallel traces running its 300 mm length. Power and
ground are on the **outside** edges (wide, high-current); signal A and B
are in the **middle** (narrow):

```
  TOP edge of PCB (one side of DIN rail flange)
  ===========================================  <- 12V trace, 5 mm wide
  
  -------------------------------------------  <- A trace, 1 mm wide
  -------------------------------------------  <- B trace, 1 mm wide
  
  ===========================================  <- GND trace, 5 mm wide
  BOTTOM edge of PCB
```

Trace centre-to-centre spacing matches pogo pin pitch on the unit:
**8 mm centre-to-centre between adjacent traces**, ~24 mm total span.

The wide 12V/GND outside traces also act as guard rails for the
differential A/B pair in the middle, giving the bus a clean
common-mode reference.

## Trace widths and current

1 oz copper, FR-4 substrate:

| Trace | Width | Capacity | Notes |
|---|---|---|---|
| 12V | 5 mm | ~5 A | Sized for 4 A peak per row with margin |
| GND | 5 mm | ~5 A | matches 12V |
| A | 1 mm | n/a | Pure signal, microamp current |
| B | 1 mm | n/a | matches A |

Trace separation: 2 mm minimum between adjacent traces.

## Plating

**ENIG (gold over nickel)** on all contact strips. Required for pogo pin
reliability — gold doesn't oxidise so contact resistance stays stable
over thousands of mating cycles. HASL also works but wears faster.

## Mechanical

| Spec | Value |
|---|---|
| PCB outline | 300 mm × ~30 mm (fits 35 mm DIN rail TS35 channel) |
| Layer count | 2-layer FR-4 |
| Thickness | 1.6 mm or 2.0 mm for rigidity |
| Mounting | **4 mounting holes** along centreline at x = 8, 100, 200, 292 mm (M3 clearance) — single-pair end mounts on a 300 mm strip allow mid-span sag and inconsistent pogo-pin contact pressure |
| Unit pitch | 37 mm (8 stations per board, 16 per row) |
| Connectors | 4-pin shrouded box header at each end (2.54 mm, indexed) |

DIN rail itself: standard 35 mm TS35, cut to row length (~600 mm). Mounts
to the case structure with screws through the rail's slotted holes.

## End connectors

Each bus PCB has a **4-pin shrouded box header at each end**. Identical
pinout on both ends:

| Pin | Net |
|---|---|
| 1 | 12V |
| 2 | RS485_A |
| 3 | RS485_B |
| 4 | GND |

Pin order **12V / A / B / GND** matches the physical top-to-bottom
trace order on the bus PCB and the pogo-pin order on the unit
underside. 12 V and GND on opposite ends keeps adjacent-pin shorts
off the supply rails.

Same connector and pinout on the master side and the daisy-chain side
means:
- A single PCB design covers all 8 board positions in the system.
- Cables are reversible (master cable = daisy-chain cable, just
  different lengths).
- Termination is a plug on the unused end of the second bus PCB.

The two on-PCB connectors are wired in parallel onto the same 4 traces —
power and signal flow straight through unimpeded.

## Pogo pin pattern (per unit)

Four pogo pins in a vertical line on the unit's underside, matching the
bus PCB trace pitch:

| Pin | Net | Pogo position |
|---|---|---|
| 1 | 12V | top (matches outer-top trace) |
| 2 | RS485_A | upper-middle |
| 3 | RS485_B | lower-middle |
| 4 | GND | bottom (matches outer-bottom trace) |

Spacing: ~8 mm between adjacent pins, ~24 mm total span.

Pogo pins are spring-loaded plungers, deflection range ~1 mm. Through-hole
mounted on the unit PCB. Tip diameter < trace width to give mating
tolerance.

## Polarization (REQUIRED — not optional)

Pogo pin pattern is symmetric top-to-bottom (12V/A/B/GND) — reversed
mounting shorts 12 V to GND through ~50 mΩ contacts at 4 A. The master
polyfuse trips, but the unit's AO3401 + SMAJ15A may take damage first.

**Mandatory mechanical key (pick one):**

1. **Asymmetric 5th polarization pogo pin** (recommended). Add a 5th
   THT pogo on the unit and a corresponding ENIG keying pad on the
   bus PCB, offset to ONE side of the 4-pin power/signal column. The
   bus PCB has the keying pad on the matching side only — reversed
   install, the 5th pogo lands on bare FR-4 and the unit physically
   does not seat. Same Mill-Max 0906-2 part. No electrical function.
2. **Hard-keyed DIN clip** — pick a DIN clip with an asymmetric snap
   profile that physically only seats one way. Document the exact
   clip MPN in `UNIT_BOM.csv`.

Belt-and-braces additions (not standalone — must be paired with #1
or #2):
- "TOP" silkscreen marker on the unit board edge.
- "BOTTOM" silkscreen marker on the bus PCB edge.

Without an enforced mechanical key, **the first wrongly-clipped unit
will damage itself**.

## Hot-swap considerations

The bus is live (12 V at up to ~4 A peak per row, RS-485 active). When a
unit is added or removed:

- Pogo pin contact bounces during make/break.
- 12V make: small inrush as unit's bulk cap (22 µF) charges. Limited by
  contact resistance (~50 mΩ), peak inrush ~10 A for ~µs. SMAJ15A TVS on
  the unit catches any kickback.
- RS-485 bus: brief disturbance during pogo contact bounce. Firmware
  retransmits on CRC fail; not data-destructive.
- Recommendation: insert/remove with row power off if practical, or
  accept brief bus glitches if hot-swap is required.

## Termination

Soldered 120 Ω resistor not on the bus PCB itself — instead, on a
**terminator plug** that mates with the unused 4-pin connector on the
last bus PCB in the chain. Plug shell contains a 120 Ω 1% resistor wired
across the A and B pins (pins 3 and 4), with 12V/GND pins unused.

This means **every bus PCB is identical** — termination is a separate
hardware element.

## Cables

| Cable | Length | Conductors | Notes |
|---|---|---|---|
| Master cable | ~30 cm (set at build) | 4 (12V/GND/A/B) | 22 AWG or shielded twisted pair |
| Daisy-chain cable | ~5-15 cm | 4 (12V/GND/A/B) | short, between bus PCBs |

**Cable wiring is straight-through (no twist or flip):**
`pin 1 ↔ 1, pin 2 ↔ 2, pin 3 ↔ 3, pin 4 ↔ 4`. Both bus-PCB connectors
share the same pinout, and both ends of any cable are wired identically.

Both cables: 4-pin female on each end (housing TBD per OPEN_DECISIONS
#4), IDC mass-terminated onto 4-conductor flat cable, or hand-crimped
onto 22 AWG round cable.

Twisted-pair preferred on A/B for noise immunity, but 4-conductor flat
cable also works at 250 kbaud over the short lengths involved.

## Terminator plug pinout

The terminator plug on the unused end of the last bus PCB:

| Pin | Net | Wired to |
|---|---|---|
| 1 | 12V | NC (do not connect) |
| 2 | RS485_A | one leg of 120 Ω 1% |
| 3 | RS485_B | other leg of 120 Ω 1% |
| 4 | GND | NC (do not connect) |

Pins 1 and 4 must remain unconnected on the terminator plug — wiring
them to anything (especially each other) shorts +12 V to GND.

## Why this beats a single 600 mm bus PCB

| Concern | 1× 600 mm | 2× 300 mm + cable |
|---|---|---|
| Fab cost (qty 4-8 boards) | premium-fab pricing, ~€20-25/row | standard JLC pricing, ~€15-18/row |
| Shipping/handling | one big board, fragile | smaller boards, sturdier |
| Repair / partial replacement | replace whole row | replace half-row |
| Cable count | 0 between sections | 1 short daisy-chain cable per row |

Daisy-chain cable is cheap and short. The fab and shipping savings
outweigh the cable cost.

## Bus PCB BOM

See `BUS_PCB_BOM.csv`. Quantities listed are per single bus PCB
(multiply by 8 for a complete 4-row system).
