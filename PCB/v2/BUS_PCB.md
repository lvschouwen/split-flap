# DIN-Rail Bus PCB

**Revision:** 2026-04-26

> **⚠ SUPERSEDED 2026-07-09 (#100 decision: card-edge).** The
> pogo-strip contact system this document describes is withdrawn. The
> board becomes a **backplane carrying one keyed 2×5 card-edge socket
> per station** (2.54 mm dual-row; see SCHEMATIC_UNIT.md CE entry and
> OPEN_DECISIONS.md 2026-07-09 entries). What carries over: JST-VH
> J_in/J_out ends, 2× ~300 mm per row daisy-chained, far-end 120 Ω
> terminator plugs, 4-net bus (12V/GND/A/B). The redesign happens
> after the #100 insertion-kinematics mock-up; treat the geometry and
> contact-strip content below as historical reference only.


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

> **⚠ GEOMETRY UNRESOLVED — issue #100**: the station pad pattern and
> unit orientation will be redesigned after a dimensioned
> cross-section + mock-up (see § Mechanical). Treat the trace/station
> geometry here as provisional.

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

**Hard/thick gold on all contact strips** ("gold fingers":
0.3–0.8 µm Au over ≥2.5 µm Ni, or ENEPIG) — **not** standard thin
immersion ENIG (~0.05 µm Au), and not HASL. The threat here is not
mating-cycle count but **fretting**: micro-motion of the pogo tips
under continuous stepper vibration wears through thin immersion gold,
exposes the nickel, and contact resistance climbs as it oxidises.
Hard gold over a thick nickel barrier survives it.

## Mechanical

| Spec | Value |
|---|---|
| PCB outline | **300 mm × 32 mm** (widened from 30 mm so trace Y-offsets land at ±12, ±4 mm from board centre exactly matching the unit pogo pin geometry — eliminates the 1 mm mechanical Y mismatch flagged in PCB review) |
| Layer count | 2-layer FR-4 |
| Thickness | 1.6 mm or 2.0 mm for rigidity |
| Mounting | **4 mounting holes** along centreline at x = 8, 100, 200, 292 mm (M3 clearance) — single-pair end mounts on a 300 mm strip allow mid-span sag and inconsistent pogo-pin contact pressure |
| Unit pitch | 37 mm (8 stations per board, 16 per row) |
| Connectors | **JST-VH 4-pin male, 3.96 mm pitch THT (B4P-VH-A, LCSC C144392)** at each end — JST-VH ≥5 A per pin matches the 4 A row fuse; mating housing VHR-4N. NOT JST-XH (3 A undersized) and NOT 2.54 mm box header (an earlier draft said this; locked across the system per OPEN_DECISIONS #4). |

DIN rail itself: standard 35 mm TS35, cut to row length (~600 mm). Mounts
to the case structure with screws through the rail's slotted holes.

> **⚠ GEOMETRY UNRESOLVED — issue #100**: the mechanical spec above
> does not work as documented. The 80 × 40 mm unit outline cannot sit
> on a 37 mm pitch (80 mm along the rail = 43 mm overlap; rotated is
> still 40 > 37 mm and puts the pogo column parallel to the traces);
> MH3 (x = 200 mm) collides with station 5 (x = 203 mm); the
> 4-standoff mounting sags ~1.5 mm under pogo preload — more than the
> pogo travel, so the board must be continuously backed; and the end
> connectors sit inside the station 0/7 unit envelopes. Station pad
> geometry and unit orientation WILL be redesigned after a
> dimensioned cross-section + mock-up. Do not lay out against these
> numbers. Do not renumber holes or stations piecemeal.

## End connectors

Each bus PCB has a **JST-VH 4-pin connector (B4P-VH-A, LCSC C144392 —
verify per #105; mating housing VHR-4N) at each end**, per the
Mechanical table above and OPEN_DECISIONS #4. Identical
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

**Four** pogo pins in a vertical line on the unit's underside,
matching the bus PCB trace pitch (no 5th polarization pin — see
Polarization section below):

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

> **⚠ GEOMETRY UNRESOLVED — issue #100**: station pad geometry and
> unit orientation will be redesigned after a dimensioned
> cross-section + mock-up (unit outline does not fit the 37 mm pitch
> — see § Mechanical). The 4-net pogo order is the electrical
> contract; the physical pattern is not final.

## Polarization (REQUIRED — not optional)

Pogo pin pattern is symmetric top-to-bottom (12V/A/B/GND) — reversed
mounting shorts 12 V to GND through ~50 mΩ contacts at 4 A. The master
polyfuse trips, but the unit's AO3401 + SMAJ15A may take damage first.

**Polarization is enforced mechanically by the unit's 3D-printed DIN
clip — LOCKED (no PG_KEY pogo, no PG_KEY ENIG pad).**

The clip is asymmetric: only one mounting orientation engages the
35 mm DIN rail correctly. A reversed unit cannot seat. Earlier drafts
(post-Gemini) added a 5th polarization pogo pin (PG_KEY) at
y = +14 mm on the unit and a matching ENIG pad on the bus PCB. That
approach was withdrawn because:
- The PG_KEY pogo at y = +14 and PG1 (12V) at y = +12 are only 2 mm
  centre-to-centre, but pogo pads are 2.45 mm — they physically
  collided.
- A spring pogo pressing on bare FR-4 is not a hard mechanical
  interlock; under axial load the unit can still seat with the
  polarization pogo squashed sideways.

The DIN clip is the only polarization mechanism. It is the user's
own design (3D-printed from a MakerWorld STL — link to be supplied
in build notes). Clip MPN equivalent: any asymmetric 35 mm TS35 clip
with a single-orientation rail engagement profile. Verify on first
article that a reversed unit physically refuses to clip on.

Belt-and-braces additions (not standalone — must be paired with the
asymmetric clip):
- "TOP" silkscreen marker on the unit board edge.
- "BOTTOM" silkscreen marker on the bus PCB edge.

Issue #104 will additionally introduce an asymmetric contact-pad
pattern on the bus PCB as an electrical soft-key.

Without an enforced mechanical key, **the first wrongly-clipped unit
will damage itself**.

## Insertion / removal — POWER OFF ONLY (mandatory)

Units MUST be inserted and removed with row power off. Hot-swap on a
live bus is **prohibited**, not merely discouraged (revised
2026-07-04, #102):

- The TPL7407L COM pin tolerates ~0.5 V/µs; hot-plug contact edges
  are 5–10 V/µs — well past the part's rating.
- An earlier draft estimated "peak inrush ~10 A for ~µs" charging the
  unit's 22 µF bulk cap. That was optimistic: the ideal-source peak
  is tens of amps, through pogo tips rated ~2 A.
- Pogo contact bounce also glitches the live RS-485 bus during
  make/break.

Procedure: power the row down (at the master or the brick), swap the
unit, power back up.

## Termination

Soldered 120 Ω resistor not on the bus PCB itself — instead, on a
**terminator plug** that mates with the unused 4-pin connector on the
last bus PCB in the chain. Plug shell contains a single 120 Ω 1%
resistor wired across the **A and B pins** of the connector — that's
**pin 2 (A) and pin 3 (B)** per the 12V/A/B/GND pinout below. Pins 1
(12V) and 4 (GND) stay unconnected.

Per the 2-bus architecture (2026-07-04, #102 — see
`ARCHITECTURE.md`), each bus serves a row pair with the master as an
**unterminated mid-bus tap**: the plug at each row's far end
terminates its end of the shared 2-row bus, and there is **no
termination at the master**. The hardware is unchanged — still one
plug per row, 4 system-wide.

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

Both cables: 4-pin female on each end (housing VHR-4N per
OPEN_DECISIONS #4, resolved), IDC mass-terminated onto 4-conductor
flat cable, or hand-crimped onto 22 AWG round cable.

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
