# Layout Guide — Bus PCB

**Revision:** 2026-07-04 (review pass #102; original 2026-04-26)

> **⚠ SUPERSEDED 2026-07-09 (#100 decision: card-edge).** The
> pogo-strip contact system this document describes is withdrawn. The
> board becomes a **backplane carrying one keyed 2×5 card-edge socket
> per station** (2.54 mm dual-row; see SCHEMATIC_UNIT.md CE entry and
> OPEN_DECISIONS.md 2026-07-09 entries). What carries over: JST-VH
> J_in/J_out ends, 2× ~300 mm per row daisy-chained, far-end 120 Ω
> terminator plugs, 4-net bus (12V/GND/A/B). The redesign happens
> after the #100 insertion-kinematics mock-up; treat the geometry and
> contact-strip content below as historical reference only.

**Companion to:** `SCHEMATIC_BUS.md` (nets, MPNs, contact pad geometry).
**Read this when:** placing components and routing in KiCad 10. See
`KICAD_HANDOFF.md` for the tool-specific workflow (project setup,
libraries, plot/Gerber, JLC integration).

> **⚠ GEOMETRY UNRESOLVED — issue #100. Do NOT lay out this board yet.**
> The 2026-07-04 review found the station geometry unbuildable as
> documented: the unit outline does not fit the 37 mm station pitch in
> any orientation, MH3 at (200, 16) collides with station 5 at x = 203
> (screw head inside the unit landing zone), 4-point mounting allows
> mid-span sag exceeding total pogo travel (continuous backing
> required), and the end connectors sit inside the station-0/7 unit
> envelopes. Station positions, contact-zone pattern, mounting scheme
> and connector placement will all be re-derived from the #100
> dimensioned cross-section + mock-up. Everything below is retained as
> the pre-review baseline only.

This is the simplest of the three boards: 2 connectors, 4 long traces,
8 contact stations, 4 mounting holes. Most of the work is artwork
(traces + custom contact zones), not component placement.

---

## Board outline

| Spec | Value |
|---|---|
| Outline | **300 × 32 mm** rectangle |
| Layers | 2 |
| Substrate | FR-4 |
| Thickness | 1.6 mm (or 2.0 mm if extra rigidity is desired) |
| Copper | 1 oz both sides |
| **Plating** | **Hard/thick gold on the contact strips — mandatory** ("gold fingers" process, 0.3–0.8 µm Au over ≥2.5 µm Ni, or ENEPIG). Standard thin ENIG (0.05–0.1 µm Au) is NOT sufficient: the threat is fretting wear under continuous stepper vibration, not mating cycles (#102). HASL wears fastest of all. |
| Solder mask | Green; **opening over every contact pad** (mask removed entirely over the pogo landing zone) |
| Silk screen | White |

Set the board origin at **top-left corner**. Y axis points down.
Board centre is at (150, 16) mm.

## Mounting holes (locked, 4 holes)

Two-hole end-only mounting allows mid-span sag of a 300 mm strip,
which makes pogo contact pressure inconsistent. Locked to 4:

| Hole | Position | Purpose |
|---|---|---|
| MH1 | (8, 16) | DIN-rail bracket, left end |
| MH2 | (100, 16) | Anti-sag mid-span |
| MH3 | (200, 16) | Anti-sag mid-span |
| MH4 | (292, 16) | DIN-rail bracket, right end |

Drill 3.2 mm (M3 clearance). NPTH (no plating).

## Trace centerlines (Y from top edge)

These Y-offsets exactly match the unit pogo pin pattern (±12, ±4 mm
from board centre at y = 16 mm) — do not change them:

| Net | Y centerline | Trace width |
|---|---|---|
| 12V  | 4 mm  | 5 mm wide pour |
| A    | 12 mm | 1 mm |
| B    | 20 mm | 1 mm |
| GND  | 28 mm | 5 mm wide pour |

Trace separation 8 mm centre-to-centre. The wide 12V / GND outer
traces also act as guard rails for the differential A/B pair.

## Stations (8 per PCB)

Pogo contact zones at:

| Station | x (centre) |
|---|---|
| 0 | 18 mm |
| 1 | 55 mm |
| 2 | 92 mm |
| 3 | 129 mm |
| 4 | 166 mm |
| 5 | 203 mm |
| 6 | 240 mm |
| 7 | 277 mm |

37 mm pitch. First station at 18 mm from left edge; last at 277 mm
from left edge → 23 mm clearance to right edge.

## Contact zone shapes (per station)

At each station, 4 widened landing zones, all on top layer, all
bare hard gold (no mask):

| Pad | Centre | Shape | Notes |
|---|---|---|---|
| 12V | (xstation, 4)  | 5 × 5 mm rectangle | extension of the 5 mm 12V trace |
| A   | (xstation, 12) | 3 × 5 mm rectangle | widened from 1 mm trace |
| B   | (xstation, 20) | 3 × 5 mm rectangle | widened from 1 mm trace |
| GND | (xstation, 28) | 5 × 5 mm rectangle | extension of the 5 mm GND trace |

Contact zones provide ±2 mm mechanical alignment tolerance for the
pogo pin tip.

**No PG_KEY 5th pad.** Polarization is enforced by the unit's
asymmetric DIN clip — see `SCHEMATIC_BUS.md` § Polarization.

## Connectors

JST-VH 4-pin male, THT (B4P-VH-A, LCSC C144392). One at each end.
Pin order **12V / A / B / GND** (pin 1 → pin 4) end-to-end.

| Connector | Position (centre) | Pin 1 facing |
|---|---|---|
| J_in  | (10, 16)  | inward (toward stations) |
| J_out | (290, 16) | inward (toward stations) |

Connectors mounted on **top side** so cables exit upward, away from
the DIN rail channel where the pogo pins land.

## Layer strategy

- **Top**: connectors, all 4 horizontal traces, all 8 × 4 = 32 contact
  zones, silk labels.
- **Bottom**: GND pour for return reference + EMI shielding. No
  signals on bottom — keep it as a continuous reference plane to
  give the differential A/B pair a clean common-mode path.

Stitch via fence around the board perimeter on bottom GND every
~10 mm.

## Routing order (recommended)

1. Place 4 mounting holes (MH1–MH4) first — they constrain everything else.
2. Place J_in at (10, 16), J_out at (290, 16).
3. Draw the 4 horizontal traces full length:
   - 12V: 5 mm wide @ y = 4
   - A: 1 mm wide @ y = 12
   - B: 1 mm wide @ y = 20
   - GND: 5 mm wide @ y = 28
4. At each of 8 stations, expand the trace into the rectangular
   contact zone (per the table above). Use the "Solid Region" tool
   to draw the rectangles, joined to the trace.
5. Open the solder mask over each contact zone (no mask covering).
6. Add bottom-side GND pour, full board outline minus a 1 mm clearance
   to the edge.
7. Add via fence around the perimeter (every ~10 mm) connecting top
   GND trace to bottom GND pour.
8. Add silk labels:
   - "0" through "7" near each station
   - "TOP" above the 12V trace
   - "BOTTOM" below the GND trace
   - Connector pin markers (1, 2, 3, 4) next to J_in and J_out
   - "v2" + revision date

## DRC settings

| Rule | Value |
|---|---|
| Min trace width | 6 mil (0.15 mm) — comfortable margin over JLC's 5 mil |
| Min clearance | 6 mil |
| Min drill | 0.3 mm |
| Min annular ring | 0.13 mm |
| Edge clearance | 1 mm to copper |

## Verification before Gerber

- [ ] Outline 300 × 32 mm.
- [ ] 4 mounting holes (not 2) at correct positions.
- [ ] 4 traces at correct Y positions (4 / 12 / 20 / 28 mm).
- [ ] 8 stations at 37 mm pitch starting at x = 18 mm.
- [ ] Each station has 4 contact zones of correct dimensions.
- [ ] **No 5th PG_KEY pad anywhere.** Anti-pattern.
- [ ] Solder mask removed over every contact zone.
- [ ] Both connectors are JST-VH 4-pin (B4P-VH-A, C144392), NOT
      JST-XH and NOT 2.54 mm box header.
- [ ] Pin 1 of J_in and J_out maps to 12V (not GND, not A).
- [ ] Hard/thick gold ("gold fingers" / ENEPIG) selected for the contact strips in fab options (NOT standard ENIG, NOT HASL) — see #102.
- [ ] Issue #100 closed and station geometry re-derived before this checklist is used.
- [ ] DRC passes.

## Fab order

| Spec | Value |
|---|---|
| Quantity | 10 (covers 4-row system × 2 boards/row + spares) |
| Lead time | standard JLC |
| Surface finish | **Hard/thick gold on contact strips** ("gold fingers" process or ENEPIG; confirm option with fab — see #102) |
| Estimated cost | ~EUR 50–70 total at qty 10 |
