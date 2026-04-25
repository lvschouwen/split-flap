# Backplane PCB v2 — locked design decisions

> **Single source of truth for the backplane PCB.** Design agents bind to this file ONLY.
>
> Created: 2026-04-25 (post-review backplane pivot per #76).
>
> Peer documents: `MASTER_DECISIONS.md` (master), `UNIT_DECISIONS.md` (unit). The backplane sits between master and units in the bus topology — anything in conflict between docs defers to `MASTER_DECISIONS.md` for system shape and `UNIT_DECISIONS.md` for the per-slot connector contract.

## System shape

- **One backplane per case**, hosting 16 unit-slots and 2 case-level RJ45s (chain in / chain out).
- **Backplane is split into 4 segments × 4 slots each**, each segment 320 mm × ~35 mm. A single 1280 mm continuous board exceeds JLC's standard fab limit (400 mm); 4 × 320 mm segments fit comfortably.
- **Inter-segment connection**: 6-pin board-to-board header pair (matching the per-slot connector pinout), carrying V48/V48/GND/GND/A/B between adjacent segments. 3 inter-segment joints per case.
- **Outer-segment ends host the case-level RJ45s** (segment-1 upstream end carries one RJ45; segment-4 downstream end carries the other).
- **2-layer PCB**, 1.6 mm FR-4, 1 oz / 1 oz copper, HASL finish.
- **No active silicon on the backplane.** Pure passive distribution: 16 connectors + 16 polyfuses + 2 RJ45s + 3 inter-segment connectors + caps + test points + (optional populated) terminator + ESD on the case-level RJ45s.

## Per-slot connector (mates with unit)

| Pin | Net | Notes |
|---|---|---|
| 1 | +48V | Paralleled with pin 3 for current capacity |
| 2 | RS-485 A | Differential pair with pin 4 |
| 3 | +48V | Paralleled with pin 1 |
| 4 | RS-485 B | Differential pair with pin 2 |
| 5 | GND | Paralleled with pin 6 |
| 6 | GND | Paralleled with pin 5 |

Connector type: **2×3 box header, 2.54 mm pitch, FEMALE (socket)** on the backplane (mates with the male pin header on the unit). Polarised/keyed shroud to prevent reverse insertion. Standard SMD or THT 2×3 part.

## Per-slot fault isolation

- **0.2 A resettable polyfuse on V48 inline to each slot.** Trips when a single unit's stepper coil shorts; rest of bus stays up. Auto-reset after fault clears. Adds ~€1.60 per backplane (16 slots × ~€0.10).
- **No per-slot data-line fuses or ESD** — the case-level RJ45 carries the externally-induced-fault risk; per-slot ESD already lives on each unit's SM712.

## Case-level RJ45 (chain in / chain out)

| Pin | T568B color | Net |
|---|---|---|
| 1 | white-orange | NC reserved |
| 2 | orange | NC reserved |
| 3 | white-green | RS-485 A (true twisted pair with pin 6) |
| 4 | blue | +48V (paralleled) |
| 5 | white-blue | +48V (paralleled) |
| 6 | green | RS-485 B |
| 7 | white-brown | GND (paralleled) |
| 8 | brown | GND (paralleled) |

Same pinout as master. Case-level RJ45s are **shielded THT** (matches master/RJ45 part choice — to be harmonised at BOM-finalize).

**Pin-1 indicator** silkscreen mark on each case-level RJ45 (consistent with master).

**Cable spec**: shielded CAT5e or CAT6 (S/FTP or F/UTP), straight-through, T568B. Max 32 m total per chain, max 3 m per hop.

## Termination & bias

- **Termination: 120 Ω footprint at the chain-end of the bus**, on segment-4 of the chain-end case. **PCBA stuffing variant**: populated only on the chain-end case backplane; DNP on all other case backplanes. Single PCB design, two BOM variants at JLC PCBA. Same pattern used for the per-board-role differentiation as scottbez1's PCBA flow.
- **No failsafe bias on the backplane.** Master is the sole bias point (1 kΩ each leg, master-side only).

## Cable shield bond

- **Backplane shield floats.** RJ45 shield can pads on backplane connect to nothing (DNP both R and C). Master is the sole shield-bond point system-wide (locked 2026-04-25 to avoid ground-loop hunting).

## Inter-segment connector

- **6-pin board-to-board, 2.54 mm pitch**, same pinout as per-slot connector (V48/V48/GND/GND/A/B) — same SKU as per-slot for inventory consolidation.
- **Mating direction**: female on segment N's downstream end mates with male on segment N+1's upstream end. Or both-female on segments + male jumper between (simpler stocking — pick at schematic capture).
- **Spacing rule**: inter-segment joint adds ~3 mm of tolerance — segment-end alignment is enforced by the case mechanical structure (bracket from mech freelancer).

## Test points (per segment)

- **4× 1 mm SMD test pads per segment**: V48, GND, RS-485 A, RS-485 B.
- Accessible from the back side (motor-side) of the case for in-situ probe access during bring-up.

## Mechanical decisions

- **Segment dimensions: 320 mm × 35 mm** (each).
- **Layer count: 2** (1.6 mm FR-4, 1 oz / 1 oz, HASL).
- **Slot pitch: 80 mm** (matches case unit-pitch from MiddleFrame remix).
- **Mounting**: 2–4× M3 isolated through-holes per segment at defined positions (e.g. 10 mm from each end + mid-points). **Mech freelancer designs the 3D-printed brackets** that screw the segment to the MiddleFrame.

## Prototype / production target

- **Prototype run: 4 segments per case × 1 case = 4 segments**, JLC PCBA assembly. Allows full-case bring-up.
- **Production**: 4 segments × N cases per system.
- **Schematic capture path**: KiCad preferred to enable KiBot + kikit pipeline.

## Design artifacts (in this directory)

- `BACKPLANE_DECISIONS.md` (this file) — source of truth.
- `BACKPLANE_DESIGN.md` — schematic-level detail.
- `BACKPLANE_MECHANICAL.md` — placement brief + mounting + segment geometry.
- `BACKPLANE_BOM.csv` — JLC-native BOM (per-segment + case-level totals).
- `BACKPLANE_BRINGUP.md` — bring-up sequence.

## Open issues (non-blocking)

- **B1** — Inter-segment connector mating-direction (M-F vs F + jumper). Decide at schematic capture.
- **B2** — Mounting hole exact positions: pending mech freelancer's bracket design.
- **B3** — Whether to put the terminator on segment-4 or on a separate small "chain-end cap" PCB. Default: segment-4 PCBA stuffing variant. Revisit if cost-of-stuffing-variant exceeds ~€5/segment.
- **B4** — Optional case-level INA219 current monitor on each backplane segment for telemetry. Per scottbez1 audit, his Chainlink Base does this. Costs ~€0.80 + 1 GPIO on master per case. Defer to v2.1 if telemetry granularity becomes an operational need.

## Segment-joint mechanical reliability (post external review)

**4 × 320 mm segmentation remains accepted.** The structural weak point of the architecture is the **mechanical reliability of rigid segment-to-segment 6-pin board-to-board connectors** under vibration, transport, and thermal cycling. Failure mode: a segment joint that physically pivots on the connector pins puts bending load on the contacts and either fails open (unit drops out of the chain) or fails intermittently (RS-485 errors that look like SI failures).

Decisions:

- **Enclosure must positively support each segment and each inter-segment joint.** Mechanical bracket design (mech freelancer, STEP exchange) must include support fixtures within ≤ 50 mm of every joint so the joint does not carry bending load. This is now a **layout-readiness gate**: backplane is not layout-ready until the bracket geometry is finalised.
- **If board-to-board headers remain**, require **keyed and latching** connectors (not generic 2.54 mm pin headers) **or dual connectors / dowels / brackets** that constrain bending so the connector pins carry only contact load.
- **Strongly consider short keyed wire harness jumpers or flex/FFC** for segment joints **if mechanical support is uncertain**. A 30–50 mm pre-crimped 6-conductor harness with keyed Molex/JST plug at each end is mechanically forgiving and cheap.

## LAYOUT_CONSTRAINT: backplane pre-layout checklist

- **Route RS-485 as a trunk** (continuous A/B pair down the long axis), **not as a star** from the inter-segment connector to each slot.
- **Per-slot RS-485 stubs ≤ 5 mm** from trunk to slot socket pin. Anything longer kills SI margin at 500 kbaud over 16 stubs.
- **Continuous GND reference plane** under the A/B pair on the opposite layer for the full segment length. No splits, no via-fence interruptions.
- **Avoid repeated long unterminated branches.** The trunk + 16 short stubs is the topology; do not introduce intermediate breakouts.
- **`R_TERM` placement only on segment-4 (chain-end case PCBA variant).** All other segments DNP. Segment PCBs are identical; differentiation is at PCBA stuffing time.
- **Optional DNP per-slot debug LED + resistor footprints** if space allows — saves bench debug time at zero cost when not populated.
- **Test pads at segment IN and segment OUT** for V48, GND, A, B (1 mm SMD), edge-accessible from the back of the case for in-situ probing.
- **Exact RJ45 footprint and inter-segment 6-pin connector footprint locked before schematic** — generic 8P8C and generic 2×3 are not acceptable. Same RJ45 SKU as master.

## REVIEW_STRONG_OPINION: harness over rigid joints

Short keyed harness jumpers are mechanically safer than rigid board-to-board segment joints unless the enclosure positively supports each segment and joint. If the bracket spec is not finalised by the time schematic capture starts, default to wire-harness segment joints.
