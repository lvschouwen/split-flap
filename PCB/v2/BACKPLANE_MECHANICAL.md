# Backplane PCB v2 — Mechanical brief

> Complement to `BACKPLANE_DECISIONS.md`. Placement brief and segment geometry for the layout designer. Created 2026-04-25.

## Segment dimensions

- **320 mm × 35 mm × 1.6 mm FR-4**, 2-layer, 1 oz / 1 oz copper, HASL finish.
- 4 identical segments per case (with one variant difference: outer segments host the case-level RJ45, mid segments do not).

## Slot pitch

- **80 mm slot center-to-center**, matching the case unit-pitch derived from the user's MiddleFrame remix.
- Per segment: 4 slots × 80 mm = 320 mm — the full segment length.
- First slot center at x = 40 mm from segment edge; last slot at x = 280 mm.
- Inter-segment joint adds zero pitch — slots line up across the case as 16 × 80 mm = 1280 mm centerline-to-centerline.

## Floorplan (per segment, top view)

```
           320 mm
   +------------------------------+
   |  J_SEG_IN [V48|A|V48|B|G|G]  |  ← inter-segment in (mid+outer segments)
   |                              |
   |  C_BULK                      |
   |                              |
   |  [F1] [F2] [F3] [F4]         |  ← per-slot polyfuses
   |                              |
   |  [J_SLOT1] [J_SLOT2] ...     |  ← 4 unit slots at 80 mm pitch
   |                              |
   |  TP_V48 TP_GND TP_A TP_B     |  ← test pads (edge-accessible)
   |                              |
   |  J_SEG_OUT [V48|A|V48|B|G|G] |  ← inter-segment out (mid+outer)
   +------------------------------+

For OUTER SEGMENTS, replace one J_SEG connector with J_RJ45 (case-level):
   - segment-1: J_SEG_IN replaced by J_RJ45_IN (chain IN to next-case-upstream)
   - segment-4: J_SEG_OUT replaced by J_RJ45_OUT (chain OUT to next-case-downstream)
   On segment-4 of the CHAIN-END case backplane only:
   - R_TERM 120Ω stuffed (PCBA variant) across A/B near J_RJ45_OUT
```

35 mm depth gives sufficient clearance for:
- The 2×3 box header socket (~10 mm protrusion above board)
- The inter-segment / RJ45 connector on opposing edge
- Polyfuses + bulk caps + V48 trace + GND pour

## Mounting holes

- **2× M3 isolated through-holes per segment** at minimum (4 if the segment shows mid-span sag in mech freelancer's bracket review).
- Default positions: x = 10 mm and x = 310 mm (10 mm from each end), centered on y = 17.5 mm. Adjust per mech freelancer's bracket design.
- Plated-through, 3.2 mm hole, 6 mm pad isolated from GND (DNP 0 Ω chassis-bond pattern available).

## Connector orientation

- **Per-slot box headers** point UP (perpendicular to backplane) — units slide vertically into them.
- **Inter-segment connectors** point HORIZONTALLY toward the adjacent segment (along the segment's long axis). 
- **Case-level RJ45s** point HORIZONTALLY OUT from the case (segment-1 outward in one direction, segment-4 outward in the other).

## Bracket interface (handed off to mech freelancer)

The PCB designer hands off the following to the mech freelancer:
1. STEP file of the assembled segment (including socket protrusion heights).
2. Mounting hole positions (x, y, diameter).
3. Inter-segment connector geometry (so brackets can leave a slot for the connector pair).
4. Case-level RJ45 face dimensions and position.

The mech freelancer then designs:
- The 3D-printed bracket that mounts each backplane segment to the MiddleFrame.
- The case-level RJ45 escapements in the case wall.
- Any cable-management features for the case-level patch cable.

## Silkscreen requirements

Top side:
- **Slot numbers**: `1` `2` `3` `4` next to each unit socket (per-segment numbering).
- **Polyfuse polarity** is N/A (resettable, non-polarized) but include trip rating: `0.2A`.
- **Test pads labeled**: `V48`, `GND`, `A`, `B`.
- **Pin-1 indicator** on case-level RJ45 (consistent with master).
- **Termination location marker**: silkscreen `R_TERM (DNP except chain-end)` near the R_TERM footprint on segment-4.
- **Title block** top-right: `SPLIT-FLAP BACKPLANE v2 SEG-N REV A`, git SHA placeholder, date placeholder.
- **Inter-segment polarity marker**: silkscreen arrow showing data-flow direction (→ chain-OUT direction).
- **Pin-1 indicator on every 2×3 box header.**

Bottom side: minimal — git SHA, fab tier note.

## Open issues

- **BM1** — Mounting hole positions and bracket geometry: pending mech freelancer.
- **BM2** — Whether segment edges should be V-cut grooves for break-off panelization at JLC, or routed-out: defer to JLC ordering.
- **BM3** — Whether the 320 mm segment length warrants additional mid-span supports (depends on bracket rigidity): defer to mech freelancer.
