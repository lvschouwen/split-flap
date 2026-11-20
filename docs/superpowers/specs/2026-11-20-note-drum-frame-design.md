# Note-size drum and unit frame: a generated 64-flap drum for the X1C — design

Issue: #673 · Epic: #676 · Status: draft (open questions at the end)

Project 2 of the Note-size wall; sibling of
`2026-11-20-note-size-flaps-design.md` (#670, flaps), which fixes the flap
outline this document builds around. #671 chooses the motor, #674 the board.

## Intent and why

A drum that holds 64 flaps and a unit frame that stacks at **41.5 mm** (column)
and **73 mm** (row) within **~95 mm depth**, with the motor inside the drum
hub. Success test: the **hand-turn test assembly** prints on the X1C, all 64
flaps sit in the drum, the drum turns freely by hand, and each flap falls onto
the stop one at a time.

## Goals

- A parametric build123d generator in `mechanics/drum/` (the `mech` check tree,
  same venv as `mechanics/flaps/`) that reads flap dimensions from
  `flaps/params.py`, never copies them.
- One drum for every #671 motor candidate: the hub bore takes a swappable
  adapter sleeve.
- A hand-turn test assembly (drum + frame, no motor) and the full parts as
  Bambu-ready 3mf (3MF core only, as for the flaps).
- Every number below is a named parameter; the #670 test print and the
  hand-turn test tune them, a rerun is the whole cost.

## Non-goals

- The board (#674) beyond its mounting points; motor choice (#671); firmware.
- Changing the flap outline, except as a lever named under Open questions.
- A cover, wall mounting system or cable routing beyond the unit.

## Inputs (from `mechanics/flaps/params.py`; read, do not guess)

| Symbol | Value | Source |
|--------|-------|--------|
| t (thickness) | 1.0 mm | `thickness` |
| W (width) | 36.0 mm | `width` |
| H (height) | 31.0 mm | `height` |
| narrow width / height | 32.5 / 16.0 mm | `narrow_width`, `narrow_height` |
| tab reach | 1.75 mm = (36.0 − 32.5)/2 | `tab_reach` |
| tab_h | 1.2 mm (tab y 1.2–2.4) | `tab_h`, `tab_y0` |
| y_p (pivot y) | 1.8 mm = 1.2 + 1.2/2 | `pivot_y` |
| L (pivot to free edge) | 29.2 mm = 31.0 − 1.8 | H − y_p |
| N, pitch | 64, 5.625 deg | `alphabet.json`, 360/64 |
| chord factor k | 0.098135 = 2·sin(π/64) | |

Envelope: 41.5 x 73 x ~95 mm (Note pitch; #670). Axis along x (the column
direction), display plane at the front.

## Assumptions (stated, each one checkable)

- A1. **Classic gravity drum:** axis horizontal along x on the display
  midline; the drum turns so the front moves down; flaps hang from pivots on
  the circle; the visible top half is the lowest flap of a stack leaning on the
  stop, the visible bottom half is the flap that just fell.
- A2. Frame walls and printed webs are multiples of the 0.4 mm line: 1.2 mm
  = 3 perimeters, 0.8 mm = 2, 0.6 mm = 1.5 (Arachne variable-width, Bambu
  Studio default). Webs under 0.6 mm are not used.
- A3. Printing round holes: FDM undersizes by roughly 0.1–0.2 mm, so the first
  hole is the pin plus 0.2 mm (diametral). This is a guess, the #670 test print
  replaces it (parameter `hole_slop`).
- A4. The 24BYJ48's output shaft is centred. The 28BYJ-48 it imitates has the
  shaft 8 mm off-centre, so this **must be measured when the sample arrives**;
  the bore formula below covers both.
- A5. Wall thickness 1.2 mm for the frame (3 perimeters); the stop bar and the
  window frame may be thicker where depth allows.

## 1. The pivot circle: a feasibility window, not a chosen number

R_p is squeezed from both sides. All bounds below use only params.py.

### 1a. Lower bound: holes must fit on the circle (chord vs hole)

Adjacent pivots are `chord = k · R_p` apart (k = 0.098135). A pivot hole of
diameter d needs a web of at least `web` mm to its neighbour:

    R_p ≥ (d_hole + web) / k

The tab rotates in its hole, so d_hole must clear the tab's rotating
cross-section (its diagonal), plus slop (A3):

| Tab form | Cross-section | d_hole | web | R_p min |
|----------|---------------|--------|-----|---------|
| Today: rectangular 1.0 x 1.2 | diag √(1.0² + 1.2²) = 1.562 | 1.562 + 0.2 = 1.76 | 0.8 | (1.76 + 0.8)/k = **26.1** |
| same, web 0.6 | | 1.76 | 0.6 | 24.1 |
| square 1.0 x 1.0 | diag 1.414 | 1.61 | 0.6 | 22.6 |
| **round pin Ø1.0** (lever, see OQ1) | Ø1.0 | 1.2 | 0.8 | 20.4 |
| round pin Ø1.0 | | 1.2 | 0.6 | **18.3** |

### 1b. Lower bound: the hub

The motor sits inside the hub, so the hub bore must clear it. With r_m the
motor body radius, o the shaft's offset from the body centre (0 if A4 holds),
c = 0.2 mm radial clearance:

    r_bore = r_m + o + c          (24BYJ48 if centred: 12 + 0 + 0.2 = 12.2, bore Ø24.4)
    R_p ≥ r_bore + w_hub + web + d_hole/2 = 12.2 + 1.2 + 0.8 + 0.6 = 14.8

(round pin, 0.8 web). Not binding: 1a is the harder bound. **A 24 mm motor
fits** in a Ø24.4 bore, with 3.9 mm radial left between hub and pivot-hole
ring at R_p = 18.5 (13.4 hub outer radius to 17.3 hole-ring inner radius). If
the shaft is offset like the 28BYJ-48, o = 8 and the bore becomes
2·(12 + 8 + 0.2) = Ø40.4 which does not fit at all (hub radius 20.2 + wall
> R_p): then the 24BYJ48 is out, the same way the 28BYJ-48 was.

### 1c. Upper bound: the row height

Row pitch 73 mm minus a 1.2 mm wall top and bottom = 70.6 mm for the
mechanism, 35.3 above and below the axis.

- Free hang (no guide): the lowest flap hangs straight down, reaching
  R_p + L below the axis; flop-over reaches about the same above: 2·(R_p + L)
  = 95.4 mm at R_p = 18.5. **Does not fit 70.6 for any R_p ≥ 6.1.**
- Guided: a floor guide and the stop keep the flaps tangential, reach
  √(R_p² + L²) = √(18.5² + 29.2²) = 34.57 each way, 2 · 34.57 = 69.1 ≤ 70.6.
  Solving `2·√(R_p² + L²) ≤ 70.6` gives **R_p ≤ 19.8**.

So the guided layout is mandatory, and a flat 73 mm row leaves 1.5 mm to spare
at R_p = 18.5.

### 1d. Result

1a (R_p ≥ 18.3 with a round pin and 0.6 web; ≥ 26.1 with today's tab) and 1c
(R_p ≤ 19.8) **intersect only if the tab becomes a round Ø1.0 pin**. That
tab change is a #670 follow-up (its spec: the outline is free to tune; the
validator already allows tab_h 1.0). With today's rectangular tab the window
is empty: see Open question 1.

**Recommended (conditional on the round pin): R_p = 18.5 mm, D_p = 37.0 mm.**

- Chord at 18.5: 0.098135 · 18.5 = **1.816 mm** between adjacent pivots;
  flap t = 1.0, so 0.816 mm per flap between neighbouring flaps where they run
  parallel (top and bottom of the circle).
- Hole ring: Ø1.2 holes with webs 1.816 − 1.2 = 0.616 mm (≥ 0.6, A2).
- Disc outer radius R_o = R_p + d/2 + 0.6 rim web = 18.5 + 0.6 + 0.6 =
  **19.7 mm** (Ø39.4), inside the 1c bound 19.8.

### 1e. A second squeeze: the guided lower pack

A guided lower pack with the tips held at the 35.3 mm floor means each hanging
flap tilts back by α with cos α = (35.3 − R_p)/L. At the bottom of the circle
the pivots are spaced horizontally, so the perpendicular spacing between
parallel tilted flaps is `chord · cos α`:

| R_p | t | L | α | spacing | clearance over t |
|-----|---|---|---|---------|------------------|
| 18.5 | 1.0 | 29.2 | 54.9° | 1.045 | **0.045 mm** |
| 18.5 | 0.8 | 29.2 | 54.9° | 1.045 | 0.245 |
| 18.5 | 1.0 | 26.0 | 49.7° | 1.173 | 0.173 |
| 18.5 | 1.0 | 24.0 | 45.6° | 1.271 | 0.271 |

`chord·cos α` peaks at 1.047 mm over all R_p (at ≈17.7), so R_p cannot buy
more. With today's flap the lower pack has about 0.05 mm clearance and will
jam. This is the hardest open risk (OQ2); the hand-turn test is the proof,
the levers are a thinner flap, a shorter flap, or a pack that does not
tilt uniformly (e.g. a curved guide). The generator keeps floor-guide shape
and `tip_floor` as parameters so the test settles it.

## 2. Drum

- Two **discs**, 2.0 mm thick, outer radius R_o = 19.7, 64 through-holes of Ø
  `pin + hole_slop` (1.2 at the recommendation) on the pitch circle R_p, at
  5.625° steps. Holes are vertical in the print (discs flat), so they come out
  round.
- Disc inner faces at x = ±(narrow_width/2 + g) = ±(16.25 + 0.25) = ±16.5,
  `g = 0.25` axial float per side. g is the smallest value that still lets the
  flaps slide without pinching under a 0.1–0.15 mm print error each side
  (tuned by the hand-turn). Outer faces at ±18.5 (2.0 thick). The tab tip, at
  ±18.0, sits 0.5 mm inside the outer face (hole stays blind to the viewer),
  and worst-case float of 0.5 mm (both sides) leaves 1.75 − 0.5 = 1.25 mm of
  tab engaged. **Drum width 37.0 mm.**
- **Hub:** a tube of bore Ø(2·r_bore) (24.4 for the 24BYJ48 if A4 holds), wall
  1.2, joining both discs (outer radius 13.4). The drum is two printed halves,
  each a disc plus half the tube, joined by a spigot joint and glue (open
  question OQ6 if screws are preferred). Half A (motor side) carries an
  internal bulkhead with the shaft coupling; half B (far side) an axle bore.
- **Motor adapter sleeve:** a separate printed insert with outer Ø24.4 and
  inner Ø = the motor body: 24 mm (24BYJ48: sleeve = 0.2 clearance only,
  or none), 15 mm + clearance (15BY25-560A), 10 mm + clearance (planetary),
  so one drum serves all three. The sleeve length is the motor length (19, 16.2,
  tbd mm).
- Hole size, rim web, disc thickness and `g` are the parameters the #670 test
  print (tab strength and size) tunes. A coupon plate of eight 8-hole
  segments with hole_slop 0.0–0.4 prints before the full discs.

## 3. Axle and bearings

- **Motor end:** the motor body is clamped to the frame wall (ears removed,
  #671) and its shaft drives half A through a D-flat coupling in the bulkhead.
  The motor shaft is therefore also the drum's bearing on that end.
- **Far end:** a fixed 3 mm steel pin pressed in the far wall projects 6 mm
  into half B's axle bore. Plain bearing: a PETG/PLA bore Ø3.2 (3 + 0.2 for the
  A3 undersizing), 6 mm long. 623 ball bearings (3 x 10 x 4) do not fit the
  1.2 mm wall plus a 4 mm boss in the 4.5 mm side budget (section 6).
- Side load: the drum weighs 64 flaps · ~1.1 g + printed halves, 100–150 g
  total (assumption: weigh it on the print). The geared 24BYJ48 output shaft is
  rated for light radial load; if the hand-turn shows play, the motor end
  gains a printed plain-bearing ring on a wall boss (the hub tube turns inside
  it), so the shaft carries only torque. OQ4.

## 4. Flap stop and release

Coordinates (z toward the back, y up): axis at (z, y) = (R_p, 0) behind the
display plane z = 0; front-most pivot P0 = (0, 0) on the midline; pivot at angle
φ above P0 is at (R_p(1 − cos φ), R_p sin φ).

The stop is a **lip along x** in front of the top of the stack. A held flap leans
forward with its free edge against the rear face of the lip; the lowest held
flap is the next to fall. As the drum turns, its pivot P drops away from the
lip's lower edge S; the flap stays held while the pivot-to-stop distance
|PS| ≤ L and is released the moment |PS| > L (its edge can no longer reach S).

**One release per pitch.** Let the held flap's overlap be o = L − |PS| (≥ 0
held). The pivot moves one chord per step, so |PS| changes by Δ ≤ chord·cos γ
(γ the angle between the pivot's travel and PS; ≈ 0 when S lies above the pivot
on the front arc). Exactly one flap is released per step and the next is still
held when the lowest held flap has

    0 < o < Δ,    Δ = chord = 1.816 mm at R_p = 18.5

I take **o = Δ/2 = 0.91 mm**: this centres the margin on the one tolerance
that matters. Reason: S position and L each print to about ±0.15–0.2 mm, so
the worst-case error is ±0.4 mm, less than o = 0.91 on one side and Δ − o =
0.91 on the other. Result: S is at |PS| = L − o = 29.2 − 0.91 = **28.29 mm**
from the pivot of the release-station flap. That flap's pivot is one pitch up
from P0: φ = 5.625°, P = (0.09, 1.81), so S = (z_s, 1.81 + 28.29) = height
**30.1 mm above the midline**, z_s = lean (below).

**Lean:** `stop_dz` is how far the stop's edge is in front of the pivot's z
(initial 1.0 mm ≈ 2° lean of a 29-mm flap). Unlike o, this one has no
derivation: it biases gravity so a released flap falls forward, not back onto
the stack; the hand-turn test tunes it.

The lip's width: full 36 mm in x with a 1.2 mm face; its lower edge radius is
printed sharp (0.2 mm edge rounding is optional, no more, to keep S
reproducible).

**Floor guide** (lower pack) per 1c/1e: a plate or curve under the drum in the
frame; its profile is a parameter. Its job is to keep every flap tip within
35.3 mm of the axis; shape settled by the hand-turn (OQ2).

## 5. Display window and visible glyph area

The front plate carries the window; its edges are the stop lip (top) and the
bottom rail.

- **Window height:** top edge is the stop lip at 30.1 mm above the midline
  minus the lip's visible face (1.2): **~28.9 mm** of flap visible above the
  midline; by symmetry about the midline the window is ≈ **57.8 mm**
  (rounded to 58.0, parameter).
- **Window width:** 34.0 mm (the flap's 36 mm less 1.0 mm per side) so the
  flap edges, which float ± 0.25 mm and vary with print, stay hidden. The window
  must not exceed the frame's inner width 41.5 − 2·1.2 = 39.1.
- **Visible glyph area:** glyph cap height = `cap_frac · 2H` = 0.70 · 62 =
  **43.4 mm** (21.7 mm above and below the midline's gap), glyph width at most
  narrow_width − 2 · edge_margin = 32.5 − 2.0 = **30.5 mm**. Inside the 34.0 x
  58.0 window: from the glyph top to the window's top edge is 28.9 − 1.8 −
  21.7 = 5.4 mm. The window clips the glyph only above a cap height of
  2 · (28.9 − 1.8) = 54.2 mm, i.e. cap_frac 0.87; cap_frac stays a #670 knob. Resolves
  the "visible window not known until project 2" note in the flaps spec.
- The 15 mm of the row outside the window (73 − 58) is the stop, the bottom
  rail and the frame's top/bottom walls; the mechanism's 70.6 mm (1c) sits
  behind that plate.

## 6. Frame: stacking and envelope

**Width (41.5 pitch).** The drum is 37.0 mm. 41.5 − 37.0 = **4.5 mm** between
the outer drum faces of neighbouring columns holds: one 1.2 mm wall per unit
(2.4), a 0.4 mm drum-to-wall gap each side (0.8): 3.2 mm used, **1.3 mm slack**
for the stacking tolerance (0.65 per unit, ≥ 0.4 mm each way for 0.2 mm print
error). So the frame is a thin shell: no room for a boss in the wall. Bearing
bosses protrude **inboard** into the hub bore (the bore is empty beyond the
motor), not outboard.

**Rows (73 mm).** Mechanism 70.6 + walls 2 · 1.2 = 73.0 mm exactly (section 1c).
At R_p = 18.5 the guided mechanism is 69.1 high, 1.5 mm to spare; rows stack
flush, a 0.2 mm printed tolerance per frame goes into that 1.5 mm.

**Depth (~95).** Front wall at z = −1.2 … 0 (window plate). Drum axis at
z = R_p = 18.5; the drum back at 18.5 + 19.7 = 38.2; flap reach on the rear
side √(R_p² + L²) = 34.6 from the axis → z = 53.1. That leaves 95 − 53.1 − 1.2 =
**~40 mm** behind the mechanism for the board (#674, ATmega328P, driver, hall,
connectors). The front of the pack is open in depth: falling flaps stay
within z ∈ [0, ~L] ahead of the stop.

**Mounting points.** Four M2 bosses on the back face for the board (hole
pattern is #674's: parameters `board_hole_xy`, default none until #674).
Frames join to neighbours by their side walls (clip or M2); the full 15 x 3
wall mount is out of scope.

## 7. Hall magnet pocket

Parameters: magnet Ø3.0 x 1.0 mm disc neodymium (assumption; the existing
unit's magnet size is the one to copy, check at implementation, OQ5), pocket
Ø3.1 deep 1.1 on the **outer face of one disc**, radius 15.5 (inside the
annulus between hub 13.4 and hole ring 17.3: 15.5 ± 1.6 = 13.9–17.1 fits),
angle θ_m a parameter. The disc is 2.0 thick: 0.9 mm floor behind the magnet.
Sensor (DRV5032/AH3144) sits on the side wall at the same radius; the gap
sensor-to-magnet is the 0.4 mm drum gap plus the wall. Final placement is
linked to the board outline (#674). Home flap and θ_m: firmware's concern
(#675 alphabet).

## 8. Print orientation (Bambu X1C, 256 x 256 mm bed)

- Drum halves: disc face down, hub tube upwards (17 mm tall, no supports; the
  tube bore is a straight cylinder). Round holes print vertical.
- Frame: back plate down, side walls and stop lip as vertical walls. The lip's
  lower edge S would be an overhang, so the lip is a separate part printed
  flat on its face (S edge on the bed, which also makes S the sharpest, most
  reproducible edge) and glued or clipped into the frame.
- Material: PETG for discs and stop, PLA acceptable for the frame. One colour
  throughout: no AMS needed. The plan has the discs print with 100 % rim
  perimeters (wall count, not infill, sets web strength).
- One plate holds one drum (two halves) + the adapter sleeve; the frame
  prints on its own plate.

## 9. Hand-turn test assembly

Drum (both halves) + frame with window, stop and floor guide, **no motor**: a
3 mm pin in the hub both ends or a coupling with a hand knob replaces the
motor. Procedure: load 64 flaps; turn the drum by hand; check (a) every flap
seats in its holes, (b) the drum turns freely (no pinch at the floor
guide, 1e), (c) each flap falls onto the stop one at a time, one per pitch
(5.625°), (d) the window shows 58 mm with legible split characters. Parameters
tuned by what fails: hole_slop, g, stop_dz, floor-guide profile.

## Approaches

1. **Python + build123d generator** — *chosen*, as for the flaps; reuses the
   `mech` venv and tree.
2. **Hand CAD in the slicer/FreeCAD** — one-off, but every flap or motor
   change reworks it; rejected, same reasoning as #670.
3. **Reuse the current 45-flap drum models** (`3dModels/Models.zip`) — built
   for 43 x 35 mm flaps and the 50 mm unit, not the Note envelope or 64
   flaps; rejected.

## Planned code layout (wave 2)

    mechanics/drum/
      __init__.py
      params.py       DrumParams (frozen, validated like FlapParams); imports
                      FlapParams for t, W, H, tab_reach, tab_h, pivot_y, so the
                      flap numbers have one source
      feasibility.py  pure: chord, 1a/1b/1c/1e bounds, release overlap. No CAD.
      drum.py         build123d: discs, hub halves, motor sleeve, magnet pocket
      frame.py        build123d: shell, window, stop lip, floor guide, bosses
      assembly.py     hand-turn test assembly (+ coupon plate of hole sizes)
      generate.py     CLI as flaps.generate: --coupon, --test-assembly, --full
      threemf.py      reuse flaps/threemf.py (import, do not copy)
      pytest.ini
      tests/          feasibility numbers pinned (R_p window, o in (0, chord),
                      chord vs hole web), params validation, holes on circle,
                      assembly parts do not intersect, bounding box inside
                      41.5 x 73 x 95, 3mf valid
      README.md

`scripts/check.sh`: `run_mech` runs the existing step for `mechanics/flaps` and
a second `step "mechanics drum pytest"` for `mechanics/drum` with the same
`MECH_VENV` (build123d and pytest pins unchanged); the `mechanics/*` case in
`touched_trees` already selects the `mech` tree. The drum tests import
`flaps.params`; run with `mechanics` on `PYTHONPATH` (pytest.ini
`pythonpath = ..`).

Wave 2 order: `params` + `feasibility` (pure, testable first) → drum → frame
→ assembly/3mf. The flap generator needs one change if OQ1 picks the round pin:
a `tab_profile` parameter in `flaps/params.py` (separate issue on #670).

## Open questions for Lucas

1. **The window is empty with today's rectangular tab** (1a needs R_p ≥ 26.1,
   1c allows ≤ 19.8). Recommended: change the flap tab to a round Ø1.0 pin
   (R_p = 18.5, hole Ø1.2, web 0.6), tested on the #670 plate before the drum.
   Alternatives: thinner web (fragile), holes in two staggered rings (the
   pivots no longer share the midline, misaligns the glyph halves), a taller
   row (breaks the Note footprint).
2. **Lower pack clearance is 0.05 mm** with today's 1.0 mm flap (1e, max
   1.047 mm spacing at any R_p). Choose the lever: thinner flap (0.8 mm gives
   0.245), shorter flap (H 27 gives ~0.17), or accept a guide that is tuned on
   the hand-turn (curved, not uniform).
3. **24BYJ48 shaft offset:** if it is off-centre (like the 28BYJ-48), the bore is
   Ø40.4 and does not fit; confirm when the sample arrives (A4).
4. **Side load on the motor shaft:** accept the drum riding on the motor's
   output bearing, or add the wall boss / plain-bearing ring (section 3).
5. **Hall magnet and sensor:** which magnet size (copy the current unit's) and
   whether the sensor sits on the wall or the board (#674).
6. **Drum joint:** spigot + glue, or M2 screws (reworkable, 4 mm of wall room).
7. **Gap tuning:** is a 1.3 mm width slack and 1.5 mm row slack enough for the
   side-by-side tolerance you expect, or should the wall go from 1.2 to 0.8?
