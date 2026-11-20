# Note-size drum and unit frame: a generated 64-flap drum for the X1C — design

Issue: #673 · Epic: #676 · Status: design settled by Lucas 2026-11-20, round 2 (encoder magnet and post, row backplane) folded in; motor-dependent questions open at the end

Project 2 of the Note-size wall; sibling of
`2026-11-20-note-size-flaps-design.md` (#670, flaps), which carries the flap
outline (0.8 mm flap, round pivot pin) this document builds around. #671 chooses the motor, #674 the board.

## Intent and why

A drum that holds 64 flaps and a unit frame that stacks at **41.5 mm** (column)
and **73 mm** (row) within the **101 mm case depth** (99.3 mm used, section 6), with the motor inside the drum
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
- The flap outline itself: the 0.8 mm thickness and the round pin are inputs owned by #670.
- A cover, wall mounting system or cable routing beyond the unit.

## Inputs (from `mechanics/flaps/params.py`; read, do not guess)

| Symbol | Value | Source |
|--------|-------|--------|
| t (thickness) | 0.8 mm | `thickness` |
| W (width) | 36.0 mm | `width` |
| H (height) | 31.0 mm | `height` |
| narrow width / height | 32.5 / 16.0 mm | `narrow_width`, `narrow_height` |
| pin reach | 1.75 mm = (36.0 − 32.5)/2 per side | `tab_reach` |
| pivot pin | round Ø1.0 mm, truncated top and bottom to t: flats 0.8 apart and 0.6 wide, so it prints flat; rotating envelope stays Ø1.0 | pin profile (#670) |
| y_p (pivot y) | 1.8 mm (value carried over; #670 owns it, L moves 1:1 with it) | `pivot_y` |
| L (pivot to free edge) | 29.2 mm = 31.0 − 1.8 | H − y_p |
| N, pitch | 64, 5.625 deg | `alphabet.json`, 360/64 |
| R_p, wall, screws | 18.5 mm, 1.2 mm, M2 | this document |
| chord factor k | 0.098135 = 2·sin(π/64) | |

Envelope: 41.5 x 73 x 99.3 mm of 101 (Note pitch; #670). Axis along x (the column
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
  hole is the pin plus 0.2 mm (diametral): Ø1.2. This is a guess, the #670 test print
  replaces it (parameter `hole_slop`).
- A4. The 24BYJ48's output shaft is centred (unconfirmed). The 28BYJ-48 it imitates has the
  shaft 8 mm off-centre, so this **must be measured when the sample arrives**;
  the bore formula below covers both.
- A5. Wall thickness 1.2 mm for the frame (3 perimeters); the stop bar and the
  window frame may be thicker where depth allows.

## 1. The pivot circle: R_p = 18.5 mm inside a feasibility window

R_p is squeezed from both sides. All bounds below use only params.py.

### 1a. Lower bound: holes must fit on the circle (chord vs hole)

Adjacent pivots are `chord = k · R_p` apart (k = 0.098135). A pivot hole of
diameter d needs a web of at least `web` mm to its neighbour:

    R_p ≥ (d_hole + web) / k

The pin rotates in its hole, so d_hole is the pin's rotating envelope (Ø1.0)
plus slop (A3): 1.2 mm.

| web | R_p min |
|-----|---------|
| 0.8 | (1.2 + 0.8)/k = 20.4 |
| 0.6 | (1.2 + 0.6)/k = **18.3** |

The web is therefore 0.6 mm (A2, one and a half perimeters). A rectangular
tab (rotating diagonal 1.56 mm, hole 1.76) would need R_p ≥ 26.1, beyond the
1c bound, which is why the pivot is a round pin.

### 1b. Lower bound: the hub

The motor sits inside the hub, so the hub bore must clear it. With r_m the
motor body radius, o the shaft's offset from the body centre (0 if A4 holds),
c = 0.2 mm radial clearance:

    r_bore = r_m + o + c          (24BYJ48 if centred: 12 + 0 + 0.2 = 12.2, bore Ø24.4)
    R_p ≥ r_bore + w_hub + web + d_hole/2 = 12.2 + 1.2 + 0.8 + 0.6 = 14.8

(0.8 web). Not binding: 1a is the harder bound. **A 24 mm motor
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

1a (R_p ≥ 18.3) and 1c (R_p ≤ 19.8) leave a window of 1.5 mm. **R_p = 18.5 mm,
D_p = 37.0 mm.**

- Chord at 18.5: 0.098135 · 18.5 = **1.816 mm** between adjacent pivots;
  flap t = 0.8, so **1.016 mm** of air between neighbouring flaps where they
  run parallel (top and bottom of the circle).
- Hole ring: Ø1.2 holes with webs 1.816 − 1.2 = 0.616 mm (≥ 0.6, A2).
- Disc outer radius R_o = R_p + d/2 + 0.6 rim web = 18.5 + 0.6 + 0.6 =
  **19.7 mm** (Ø39.4), inside the 1c bound 19.8.

### 1e. The guided lower pack and the flap thickness

A guided lower pack with the tips held at the 35.3 mm floor means each hanging
flap tilts back by α with cos α = (35.3 − R_p)/L. At the bottom of the circle
the pivots are spaced horizontally, so the perpendicular spacing between
parallel tilted flaps is `chord · cos α`:

| R_p | t | L | α | spacing | clearance over t |
|-----|---|---|---|---------|------------------|
| 18.5 | 0.8 | 29.2 | 54.9° | 1.045 | **0.245 mm** |

`chord·cos α` peaks at 1.047 mm over all R_p (at ≈17.7), so R_p cannot buy
more; the flap thickness is what sets the clearance. t must stay below 1.04 mm
for the pack to open at all, and 0.8 leaves 0.245 mm for print error and the
curve of the guide. The generator keeps floor-guide shape and `tip_floor` as
parameters; the hand-turn test confirms it (section 9).

## 2. Drum

- Two **discs**, 2.0 mm thick, outer radius R_o = 19.7, 64 through-holes of Ø
  `pin + hole_slop` (1.2 at the recommendation) on the pitch circle R_p, at
  5.625° steps. Holes are vertical in the print (discs flat), so they come out
  round.
- Disc inner faces at x = ±(narrow_width/2 + g) = ±(16.25 + 0.25) = ±16.5,
  `g = 0.25` axial float per side. g is the smallest value that still lets the
  flaps slide without pinching under a 0.1–0.15 mm print error each side
  (tuned by the hand-turn). Outer faces at ±18.5 (2.0 thick). The pin tip, at
  ±18.0, sits 0.5 mm inside the outer face (hole stays blind to the viewer),
  and worst-case float of 0.5 mm (both sides) leaves 1.75 − 0.5 = 1.25 mm of
  pin engaged. Neither the width nor the disc holes depend on the flap
  thickness (t only sets the air between flaps, 1.016 mm at the top and
  bottom, and the lower-pack clearance of 1e). **Drum width 37.0 mm.**
- **Hub:** a tube of bore Ø(2·r_bore) (24.4 for the 24BYJ48 if A4 holds), wall
  1.2, joining both discs (outer radius 13.4). The drum is two printed halves,
  each a disc plus half the tube, joined by **M2 screws**: three lugs (5 x 4 mm,
  120° apart) on the inside of the tube at the joint plane `joint_x`, clearance
  hole Ø2.2 in half A's lugs and a Ø1.7 pilot (self-tapping, M2 x 8) in half B's;
  a spigot centres the halves. The lugs sit beyond the motor: the hub interior is
  2 · 16.5 = 33.0 mm, the 24BYJ48 takes 19 and the bulkhead 1.2, leaving 12.8
  mm for the two 4 mm lug layers. Half A (motor side) carries an internal
  bulkhead with the shaft coupling; half B (far side) an axle boss with the magnet pocket (section 7).
- **Motor adapter sleeve:** a separate printed insert with outer Ø24.4 and
  inner Ø = the motor body: 24 mm (24BYJ48: sleeve = 0.2 clearance only,
  or none), 15 mm + clearance (15BY25-560A), 10 mm + clearance (planetary),
  so one drum serves all three. The sleeve length is the motor length (19, 16.2,
  tbd mm).
- Hole size, rim web, disc thickness and `g` are the parameters the #670 test
  print (pin strength and size) tunes. A coupon plate of eight 8-hole
  segments with hole_slop 0.0–0.4 prints before the full discs.

## 3. Axle and bearings

- **Motor end:** the motor body is clamped to the frame wall (ears removed,
  #671) and its shaft drives half A through a D-flat coupling in the bulkhead.
  The shaft carries torque only: the drum's radial load rides on a **printed
  plain-bearing ring** on the motor-side wall. The ring is a cylinder on the
  wall's inner face, axis along x, inner Ø39.8 (the disc rim Ø39.4 plus 0.4
  running clearance), 1.2 mm wall (outer Ø42.2, inside the 70.6 mm row), 2.0
  mm long: it runs from the wall's inner face at x = ±18.9 to ±16.9, so it
  overlaps the disc's rim (x 16.5 to 18.5) by 1.6 mm. It sits inside the 0.4 mm
  drum-to-wall gap plus the disc's own thickness, so it adds nothing outboard
  (section 6), and it clears the flaps (bodies end at ±16.25, pins sit inside
  the hole ring at R_p). The rim and ring are printed PETG on PETG; if the
  hand-turn shows drag, the ring bore opens by 0.1 (parameter `ring_clear`).
- **Far end:** a fixed **printed post** on the far wall plate carries the drum. A steel
  pin would sit on the axis where the encoder magnet and sensor (section 7) must go, and
  steel next to the magnet would disturb its field. Ø9.0 PETG journal, 6.0 mm engaged in
  half B's axle boss (bore Ø9.4: 9.0 + 0.2 for the A3 undersizing + 0.2 running
  clearance), printed in one piece with the far wall plate (section 7). Plain bearing, PETG on
  PETG like the motor-side ring; the Ø9 journal carries the same side load at lower
  pressure than the 3 mm pin did. If the hand-turn shows drag or wear, the fallback is
  the motor-side ring mirrored on the far wall (`ring_clear`) with the post only
  centring the sensor. 623 ball bearings (3 x 10 x 4) still do not fit the 1.2 mm wall
  in the 4.5 mm side budget (section 6).
- Side load: the drum weighs 64 flaps · ~0.9 g + printed halves, 100–150 g
  total (assumption: weigh it on the print), carried by the ring at the motor
  end and the printed post at the far end.

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
35.3 mm of the axis; shape settled by the hand-turn (1e).

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
error). So the frame is a thin shell: no room for a boss in the wall. The
far-end post and its boss (section 7) protrude **inboard** into the hub bore
(the bore is empty beyond the motor), and the motor-side bearing ring (section 3) sits inboard of
the wall, not outboard.

**Rows (73 mm).** Mechanism 70.6 + walls 2 · 1.2 = 73.0 mm exactly (section 1c).
At R_p = 18.5 the guided mechanism is 69.1 high, 1.5 mm to spare; rows stack
flush, a 0.2 mm printed tolerance per frame goes into that 1.5 mm.

**Depth (101 mm case, #675).** The stack runs from the front wall to the backplane,
z from the display plane; the unit board spec (#674) counted it at 101.8 mm against 101
with no rear cover. It closes as follows (changes from that count in the last column):

| Layer | z from | z to | Change |
|-------|--------|------|--------|
| Front wall (1.2), mechanism, flap tips (axis z = R_p = 18.5; rear reach 18.5 + 34.6) | -1.2 | 53.1 | none |
| Clearance, flap tips to PSU | 53.1 | 53.7 | 1.0 to 0.6 |
| PSU module (UHP-200-12 class) over the boards of 5 top-row bays | 53.7 | 79.7 | none |
| Insulating sheet (0.5 mm film) | 79.7 | 80.2 | 1.0 to 0.5 |
| Unit-board component gate | 80.2 | 88.2 | none (8 mm; C11 <= 5 is the tallest) |
| Board | 88.2 | 89.8 | none |
| Rear standoff: plug header body 2.5 + 0.2 | 89.8 | 92.5 | 3.1 to 2.7 |
| Back wall (1.2), with the `plug_slot` | 92.5 | 93.7 | none |
| Socket protruding past the wall (4.0 body, 1.2 of it sunk into the slot) | 93.7 | 96.5 | socket recessed |
| Backplane (1.6) = the rear skin | 96.5 | 98.1 | none |

Total -1.2 to 98.1 = **99.3 mm, 1.7 mm spare in 101** (was 101.8). Three changes: the
socket's body sits in the wall's `plug_slot` (the slot grows to the socket outline,
placeholder 10 x 4 mm still holds), the header body sits flush against the socket face
(the pin stub below the board falls from ~6.8 to ~6.2 mm, still a stock length), and
0.9 mm comes out of the two clearances (flap tips are guided inside the frame and never
reach the PSU side; the sheet is a film, not a board). **No rear cover on the unit:** the
backplane is the rear skin of a row; its parts face the unit, nothing is exposed behind
it. If the case needs a back panel (#675), it goes behind the backplane at z 98.1,
at most 1.0 mm thick leaving 0.7 mm; a thicker one needs the PSU module recessed or
the 26 mm part swapped. Frames without a PSU above them have 26 mm more room, but the
frame is one design. The drum spec's `depth_budget` parameter (101.0) and a stack test
pin these numbers.

The front of the pack is open in depth: falling flaps stay within z in [0, ~L] ahead
of the stop. Behind the mechanism the board has z 54 to 92.5 (#674: ATmega328PB, driver,
encoder connector, plug).

**Mounting points and the row backplane.** Four M2 bosses on the back plate's inner
face for the board (hole pattern is #674's: parameters `board_hole_xy`, default none
until #674). Each frame's back plate also carries (#674 section 6):

- a slot `plug_slot` (placeholder 10 x 4 mm, for a 1.27 mm 1 x 6 header until the plug family is picked) through
  which the board's plug reaches the row backplane;
- two registration bosses (Ø1.7 pilots for M2 x 6 self-tapping screws) at `peg_x` either
  side of the slot; the backplane has one round hole and one slot along y for them.

The backplane lies in the ~4.4 mm strip behind the back wall (z 93.7 to 98.1) (#675: 101 mm case; see Depth above: the socket
sits in the wall slot, the backplane is the rear skin) and clamps the frames of a row: the
column pitch is the backplane's hole pitch (41.5 mm +-0.1 mm), not the sum of 15 print
tolerances; the 0.65 mm of side slack per unit (above) absorbs the difference. Frames
join to neighbours by their side walls (clip or M2); the full 15 x 3 wall mount is out
of scope.

## 7. Encoder magnet, post and wiring groove

The hall is gone (#674 section 5): an AS5600-class angle sensor reads a **diametric
magnet on the drum's far axle end**. The magnet turns with the drum on its axis; the
chip sits on a fixed post, coaxial and facing it. There is no room outboard of the far
wall (section 6), so both live inside the far-end axle boss, where the hub is empty
beyond the motor.

### Stack-up (x measured inward from the far wall's inner face, mm)

| Feature | from | to | Note |
|---------|------|----|------|
| Drum-to-wall gap | 0 | 0.4 | the disc's outer face is at 0.4 |
| Far disc | 0.4 | 2.4 | 2.0 thick |
| Post journal in the boss bore | 0.4 | 6.4 | 6.0 engaged; `post_d` 9.0, bore 9.4 |
| Sensor board (0.8) and chip (1.75) on the post tip | 6.4 | 8.95 | round Ø8.4 board |
| Air gap, chip to magnet | 8.95 | 9.95 | `encoder_gap` 1.0; AS5600 range 0.5 to 3 mm, datasheet-verify |
| Magnet Ø6.0 x 2.5 in a Ø6.1 pocket | 9.95 | 12.45 | `magnet_d`, `magnet_t` (Ø5 x 2 fallback) |
| Pocket floor (printed cap) | 12.45 | 13.65 | 1.2 mm |

The boss is Ø12.0 (bore 9.4 plus 1.3 per side) and runs from the disc's outer face to
13.65: it projects 11.25 mm past the disc's inner face (2.4) into the hub. The hub's
free length beyond the motor is 12.8 mm (section 2: 33.0 - 19 - 1.2), so **1.55 mm is
left** to the motor and bulkhead zone. Radially the boss is 6.0 against the lugs' inner
radius 7.2 (5 mm lugs on the 12.2 mm bore radius): **1.2 mm spare**. The magnet's back
face is 2.75 mm from the motor's steel can and the can sits behind the magnet, away from
the chip; its effect on the angle error is a hand-turn measurement (AGC and linearity),
not a calculation. Both margins are tight: if they do not close, the fallback is the
Ø5 x 2 magnet (0.5 mm shorter) and a thinner cap. This section closes on paper only;
the hand-turn assembly decides.

### Post and wiring groove

- **Post:** one piece with the **far wall plate**, Ø9.0 PETG, axis along x. The plate
  is a separate part (the post cannot be one piece with a wall already closed around the
  drum, and it reaches 9 mm inboard past a 0.4 mm side gap): the drum is seated on the
  motor-side shaft coupling and ring, the boss slides over the post as the plate closes
  against the frame, and the plate is fixed with M2 screws or clips. It prints plate-down
  with the post standing, so the journal is round with no overhang.
- **Sensor board:** round Ø8.4 mm in a shallow pocket at the post tip, chip facing the
  magnet, held by its own FFC and a dab of adhesive.
- **FFC:** a 5-way 0.5 mm FFC (~0.3 mm thick) leaves the board's back through a 1.0 x 4.5
  mm slot in the post, opens sideways at the post's base into a **groove in the far
  wall's inner face** (5.5 mm wide, 0.6 mm deep, leaving a 0.6 mm wall, A2) and runs
  straight back (+z) from the axis to the back edge, ~70 mm, to the board's J3. It
  crosses the rotating disc's face inside 0.4 + 0.6 = 1.0 mm of clearance; the disc's
  outer face is flat (the pin holes are blind), so nothing catches. The fold at the post
  base (about 1 mm radius) is the tight spot: check it on the hand-turn assembly.
- **Magnet:** diametrically magnetised, glued in its pocket, never magnetised along the
  axis (an axial magnet reads nothing); the pole orientation is irrelevant to the sensor
  but the unit stores a zero for the assembly (#674 section 5).

## 8. Print orientation (Bambu X1C, 256 x 256 mm bed)

- Drum halves: disc face down, hub tube upwards (17 mm tall, no supports; the
  tube bore is a straight cylinder). Round holes print vertical.
- Frame: back plate down, side walls and stop lip as vertical walls; the
  bearing ring is a 2 mm stub on the motor-side wall (its top arc is a short
  overhang, printed with a support or as a separate ring glued on). The lip's
  lower edge S would be an overhang, so the lip is a separate part printed
  flat on its face (S edge on the bed, which also makes S the sharpest, most
  reproducible edge) and glued or clipped into the frame. The far wall is a separate plate printed
  flat with the post standing; it carries the FFC groove; the back plate carries the plug slot and
  the registration bosses (section 6).
- Material: PETG for discs and stop, PLA acceptable for the frame. One colour
  throughout: no AMS needed. The plan has the discs print with 100 % rim
  perimeters (wall count, not infill, sets web strength).
- One plate holds one drum (two halves) + the adapter sleeve; the frame
  prints on its own plate.

## 9. Hand-turn test assembly

Drum (both halves) + frame with window, stop and floor guide, **no motor**: a
3 mm pin in the hub at the motor end or a coupling with a hand knob replaces the
motor; the far end runs on the real printed post (a printed slug stands in for the
sensor board) with a real or dummy magnet in the pocket, and the FFC groove is checked
with a spare FFC. Procedure: load 64 flaps; turn the drum by hand; check (a) every flap
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
      drum.py         build123d: discs, hub halves, motor sleeve, axle boss with magnet pocket
      frame.py        build123d: shell, window, stop lip, floor guide, bosses,
                      post + FFC groove, plug slot, registration bosses
      assembly.py     hand-turn test assembly (+ coupon plate of hole sizes)
      generate.py     CLI as flaps.generate: --coupon, --test-assembly, --full
      threemf.py      reuse flaps/threemf.py (import, do not copy)
      pytest.ini
      tests/          feasibility numbers pinned (R_p window, o in (0, chord),
                      chord vs hole web), params validation, holes on circle,
                      assembly parts do not intersect, bounding box inside
                      41.5 x 73 x 95, 3mf valid, the section 7 stack-up
                      closes inside the free hub length, groove inside the wall
      README.md

`scripts/check.sh`: `run_mech` runs the existing step for `mechanics/flaps` and
a second `step "mechanics drum pytest"` for `mechanics/drum` with the same
`MECH_VENV` (build123d and pytest pins unchanged); the `mechanics/*` case in
`touched_trees` already selects the `mech` tree. The drum tests import
`flaps.params`; run with `mechanics` on `PYTHONPATH` (pytest.ini
`pythonpath = ..`).

Wave 2 order: `params` + `feasibility` (pure, testable first) → drum → frame
→ assembly/3mf. The flap generator (#670) carries the round pin and the 0.8 mm thickness
(`flaps/params.py`); the drum reads both from there.

## Open questions for Lucas (all motor-dependent)

1. **24BYJ48 shaft offset:** if the shaft is off-centre (like the 28BYJ-48),
   the bore is Ø40.4 and does not fit; confirm when the sample arrives (A4).
2. **1/16 batches:** a batch with a 1/16 gearbox changes `STEPS`, a protocol
   constant (#674 section 4); reject on arrival or accept the shared change.
3. **Motor side:** which side wall carries the motor (the bearing ring goes on that
   wall; the encoder post, the groove and the far-end bearing go on the other).
4. **Encoder magnet, post and groove:** the magnet size (Ø6 x 2.5 mm reference, Ø5 x 2
   mm fallback), the sensor-to-magnet gap and the stack-up of section 7 are unverified,
   and the AS5600-class figures are datasheet-verify (#674 section 5). The far-end
   bearing is a printed post on a separate far wall plate, not a steel pin: confirm that
   direction and the assembly order (drum first, plate closes over the boss).
5. **Backplane slot and registration bosses:** the plug family and stack height (#674
   section 6) fix the slot size; `plug_slot` is a placeholder until then.
