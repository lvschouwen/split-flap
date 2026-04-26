# Bus PCB — Schematic + Layout Specification

**Revision:** 2026-04-26

EasyEDA-ready specification for the DIN-rail bus PCB. Simplest of the
three boards: 2 connectors, 4 long traces, 16 contact stations,
mostly hand-drawn artwork.

PCB is functionally just routing — there is no real "schematic" in
the traditional sense. EasyEDA workflow is: draw the connectors in
schematic, wire them to net labels, then do all the real work in the
PCB editor by hand-drawing traces and contact pads.

**One PCB design, used 8 times in a 4-row system.**

## Schematic

Two components only:

| Ref | Component | EasyEDA library | LCSC# |
|---|---|---|---|
| J_in | **JST-VH 4-pin male, 3.96 mm pitch THT (B4P-VH-A)** | Connector / JST | **C144392** (CHECK — locked across system per OPEN_DECISIONS #4) |
| J_out | Same as J_in | same | C144392 |

Net wiring:

```
J_in.1 (12V) ─┬─── J_out.1 (12V)    via wide top-edge trace
               
J_in.2 (A)   ─┬─── J_out.2 (A)      via narrow upper-middle trace
               
J_in.3 (B)   ─┬─── J_out.3 (B)      via narrow lower-middle trace
               
J_in.4 (GND) ─┬─── J_out.4 (GND)    via wide bottom-edge trace
```

Pin order is **12V / A / B / GND** end-to-end so the 4 pin positions
match the physical top-to-bottom trace order on the PCB and the
top-to-bottom pogo-pin order on the unit underside. 12 V and GND on
opposite ends prevents adjacent-pin shorts from bridging the supply.

That's the whole schematic. No active components, no passives. The
intelligence is in the layout.

## PCB outline + layer stackup

| Parameter | Value |
|---|---|
| Outline | **300 mm × 32 mm** (widened from 30 mm so trace Y-offsets land at ±12, ±4 mm from board centre, matching unit pogo pin geometry exactly) |
| Layer count | 2 |
| Substrate | FR-4 |
| Thickness | 1.6 mm (or 2.0 mm for extra rigidity) |
| Copper weight | 1 oz on both sides |
| **Plating** | **ENIG** (required for pogo pin reliability — gold tip on gold pad gives stable contact resistance) |
| Solder mask | Green, opening over the contact strips |
| Silk screen | White, mark each contact station "0" through "7" and "TOP" + "BOTTOM" for orientation |

## Trace layout (top-down view)

```
                            300 mm
   +─────────────────────────────────────────────────────────+
   │                                                         │
   │ J_in   ====================================== J_out    │
   │  ●─────────────── 12V trace (5 mm wide) ─────────●      │ <-- TOP edge
   │                                                         │
   │       ████  ████  ████  ████  ████  ████  ████  ████   │ <-- 12V pogo contact zones (8x)
   │                                                         │
   │  ●─────────────── A trace (1 mm wide) ────────────●     │
   │       ░░    ░░    ░░    ░░    ░░    ░░    ░░    ░░    │ <-- A pogo contact zones (8x)
   │  ●─────────────── B trace (1 mm wide) ────────────●     │
   │       ░░    ░░    ░░    ░░    ░░    ░░    ░░    ░░    │ <-- B pogo contact zones (8x)
   │                                                         │
   │       ████  ████  ████  ████  ████  ████  ████  ████   │ <-- GND pogo contact zones (8x)
   │  ●─────────────── GND trace (5 mm wide) ──────────●     │ <-- BOTTOM edge
   │                                                         │
   │  M H        (8 unit slot positions, 37 mm pitch)    M H │ <-- Mounting holes
   +─────────────────────────────────────────────────────────+
                  ●  ●  ●  ●  ●  ●  ●  ●
                station: 0  1  2  3  4  5  6  7
```

## Trace widths and spacing

| Trace | Width | Reason |
|---|---|---|
| 12V (top) | 5 mm | 4 A peak current, ~5 A capacity on 1 oz copper |
| GND (bottom) | 5 mm | matches 12V |
| A (upper middle) | 1 mm | pure signal, microamps |
| B (lower middle) | 1 mm | matches A |

Trace centre-to-centre vertical spacing: **8 mm** between adjacent
trace centres. So (board centre at y = 16 mm from top edge):
- 12V centre: 4 mm from top edge (= +12 mm from board centre)
- A centre: 12 mm from top edge (= +4 mm from board centre)
- B centre: 20 mm from top edge (= -4 mm from board centre)
- GND centre: 28 mm from top edge (= -12 mm from board centre)

PCB total height: **32 mm** (4 mm symmetric margin above 12V trace and
below GND trace). Trace Y-offsets ±12, ±4 mm from board centre exactly
match the unit pogo pin geometry in `SCHEMATIC_UNIT.md` — no mechanical
Y mismatch. **No PG_KEY pad** — polarization is enforced by the unit's
asymmetric DIN clip, not by an extra pogo on the bus.

A/B trace gap (centre-to-centre): 8 mm. This is wider than necessary
for differential signaling at 250 kbaud — chosen for pogo pin
mechanical pitch matching.

## Contact pad geometry (custom footprint)

At each of 8 unit positions per board, all 4 traces have a **widened
contact zone** for the pogo pin to land on. EasyEDA workflow: hand-
draw these as copper rectangles, no mask covering the pad area.

| Pad | Y position (from top edge) | Contact zone shape | Connected to | Plating |
|---|---|---|---|---|
| 12V | 4 mm | 5 × 5 mm rectangle (just expand the trace) | 12V trace | ENIG, no mask |
| A | 12 mm | 3 × 5 mm rectangle (widen from 1 mm trace) | A trace | ENIG, no mask |
| B | 20 mm | same as A | B trace | ENIG, no mask |
| GND | 28 mm | same as 12V | GND trace | ENIG, no mask |

**No PG_KEY pad.** Earlier drafts (post-Gemini, pre-ChatGPT) added a
5th isolated ENIG pad at y = 2 mm for a polarization pogo on the unit.
That approach was withdrawn:
- The PG_KEY pogo at y = +14 mm and PG1 at y = +12 mm are only 2 mm
  centre-to-centre, but pogo pads are 2.45 mm — the pads physically
  collided.
- A spring pogo pressing on bare FR-4 is not a hard mechanical
  interlock — under axial load the unit can still seat with the pogo
  squashed sideways.
- Polarization is enforced by the unit's asymmetric 3D-printed DIN
  clip, which only physically engages the rail in one orientation.

The widened contact zones give pogo-pin mechanical alignment
tolerance (~±2 mm) without missing the pad.

Each contact zone is **bare copper with ENIG plating, no solder mask
opening required** (mask is removed over the entire contact zone).

## Unit station spacing

| Parameter | Value |
|---|---|
| Stations per board | 8 |
| Pitch (centre-to-centre) | 37 mm |
| First station from left edge | 18 mm |
| Last station from right edge | 18 mm (margin 18 mm + 7×37 mm = 277 mm last-station X; 300 − 277 − 18 = 5 mm right-edge clearance, comfortable fit) |

Across two daisy-chained boards: 16 stations total at 37 mm pitch =
600 mm row span.

## Connector positions

| Connector | Position | Pin 1 facing |
|---|---|---|
| J_in | left edge, centred vertically | inward (toward stations) |
| J_out | right edge, centred vertically | inward (toward stations) |

Connectors are mounted on the **top side** of the PCB so the cables
project upward and don't fight the DIN rail mounting. Pogo pins from
units come from below (units mounted on DIN rail, bus PCB above
units within the rail channel).

## Mounting holes

| Hole | Position | Purpose |
|---|---|---|
| MH1 | 8 mm from left edge, on PCB centreline | DIN rail bracket attachment |
| MH2 | 8 mm from right edge, on PCB centreline | DIN rail bracket attachment |

Hole diameter: 3.2 mm (M3 clearance).

## Layer rules

| Rule | Value |
|---|---|
| Min trace width | 6 mil (0.15 mm) — comfortable margin over JLC's 5 mil minimum |
| Min clearance | 6 mil |
| Min drill | 0.3 mm |
| Min annular ring | 0.13 mm |

## EasyEDA build steps

1. Create new EasyEDA Pro project: `splitflap-bus-v2`.
2. In schematic editor:
   - Place 2× JST-VH 4-pin male THT headers (B4P-VH-A,
     **LCSC C144392** — locked; ≥5 A per pin to match 4 A row fuse).
   - Wire pins: J_in.1 ↔ J_out.1 (label "12V"), J_in.2 ↔ J_out.2
     ("RS485_A"), J_in.3 ↔ J_out.3 ("RS485_B"), J_in.4 ↔ J_out.4
     ("GND"). Pin order: 12V / A / B / GND end-to-end.
3. Convert to PCB.
4. In PCB editor:
   - Set board outline: **300 × 32 mm**.
   - Place J_in at (10, 16) mm and J_out at (290, 16) mm.
   - Hand-route each net as a single trace at the specified width.
     Use the "Track" tool, set width per trace.
   - Add contact zones at each of 8 station positions (centred at
     x = 18, 55, 92, 129, 166, 203, 240, 277 mm).
   - Add **4 mounting holes** along centreline (y = 16 mm) at
     x = 8, 100, 200, 292 mm — M3 clearance (3.2 mm). Two end-only
     holes leave a 300 mm strip prone to mid-span sag, which makes
     pogo contact pressure inconsistent. Four-hole pattern is the
     locked spec (matches `BUS_PCB.md`).
   - Add silk screen labels: "0" through "7" near each station, "TOP"
     above the 12V trace, "BOTTOM" below the GND trace.
4. In EasyEDA's fab options, set:
   - Surface finish: ENIG (not HASL).
   - Layer count: 2.
   - Thickness: 1.6 mm.
5. Run DRC, fix any errors.
6. Export Gerber.
7. Order from JLCPCB. Quantity: 10 (covers 4-row system + spares).

## Estimated fab cost

JLC, qty 10, 300×32 mm, 2-layer, 1.6 mm, ENIG, 1 oz copper:
~EUR 50-70 total (~EUR 5-7 per board). Without ENIG (HASL): ~EUR 25-35.
ENIG is the cost premium for pogo reliability — worth it.

## Custom footprint summary

The "footprint" for each unit station is just a copper area on layer
1 with mask opening, drawn manually. No KiCad symbol needed —
EasyEDA's "Track" + "Solid Region" tools cover it.

## Bus PCB ASCII reference card

```
TOP edge
+============+========================+============+
| 12V trace  |  station contact zones | 12V trace  |
+============+========================+============+
|            ░░    A trace zones    ░░             |
|     A trace ─────────────────────                |
|     B trace ─────────────────────                |
|            ░░    B trace zones    ░░             |
+============+========================+============+
| GND trace  |  station contact zones | GND trace  |
+============+========================+============+
BOTTOM edge

   J_in ●                                     ● J_out
        |                                     |
        |  ●  ●  ●  ●  ●  ●  ●  ●            |
        |     8 stations at 37 mm pitch       |
```
