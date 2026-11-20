# Note-size flaps: a generated 64-flap set for the X1C — design

Issue: #670 · Status: approved

## Intent and why

The current walls (5 and 16 units) are bulky: the unit frame (50 mm wide,
118–138 mm tall) sets the pitch, not the flap. The goal of the larger effort is
a wall with the **Vestaboard Note footprint**: 3 rows × 15 columns in
623 × 219 × 101 mm (frameless), so ~**41.5 mm column pitch** and **73 mm row
pitch**, with **64 flaps per drum** including colour chips. Lucas chose to
match the Note exactly (not to keep today's 43 × 35 mm flaps).

The effort splits into four projects, done in order, each with its own design:

1. **Flaps** (this document) — generated print files, proven on the X1C.
2. Drum + unit frame + motor mount (28BYJ-48; its 42 mm ear span exceeds the
   column pitch, so the ears turn vertical).
3. A new unit board (new-PCB work is reopened for this purpose).
4. Firmware for the 64-flap alphabet (`SFP_ALPHABET` in
   `firmware/v2/shared/SplitFlapProtocol.h`, the I2C contract, the HA contract).

This document covers only project 1. Its success test: a test plate printed on
the X1C with two AMS comes off with legible characters, clean inlays, intact
pivot tabs, and the full 64-flap plates are ready to slice.

## Goals

- A parametric Python generator that writes the complete 64-flap set as
  Bambu-ready `.3mf` plates, with each colour as its own part so Bambu Studio maps
  parts to AMS slots.
- A small **test plate** that exercises the hard cases, so printer limits are
  learned before a full set is printed.
- All dimensions as named parameters in one place; the drum project will change
  them, and a rerun must be the whole cost of that change.
- Reproducible output: the font is checked in, Python deps are pinned exactly
  (repo rule #648).

## Non-goals

- The drum, frame, motor mount, board, firmware alphabet — projects 2–4.
- Changing the deployed 45-flap units or `3dModels/Models.zip` (the original
  Printables archive stays as is).
- A GUI, or slicer profiles beyond what the 3mf carries.

## Anti-goals

- A flap that only prints at a size the Note grid cannot hold (≤ 41.5 mm column
  pitch, ≤ 73 mm row pitch for two flap halves plus frame).
- Hand-edited per-character geometry: every flap comes from the same code path.
- Output that changes between runs on the same inputs (a font fetched from the
  web, unpinned CAD libs).

## Constraints

- **Printer:** Bambu Lab X1 Carbon, 256 × 256 mm bed, **two AMS = 8 slots**.
  Palette: black (flap body), white (characters), red, orange, yellow, green,
  blue, violet = exactly 8.
- **Reference geometry** (original `Flap_blank_do_not_print.stl`, measured): 43 ×
  35 × 1.0 mm. Lower 18 mm (hinge side) is 39 mm wide; two pivot tabs 2.0 mm
  out × 1.2 mm tall at y = 1.2–2.4 mm from the hinge edge; upper part full 43 mm.
  Characters are inlaid 0.4 mm on both faces as separate parts (`FlapN_1`,
  `FlapN_2` in the archive). The tab shape fits the current drum; the new drum
  (project 2) will be designed to the new flap, not the reverse.
- **Target flap:** ~**36 × 31 mm**, 1.0 mm thick by default, same outline scheme
  (narrow hinge zone + tabs, full width above). Tabs scale less than linearly:
  keep ≥ 1.0 mm tab height and ≥ 1.5 mm tab reach so they survive.
- **Split-character rule:** character *i* shows its **top half on the front of
  flap i** and its **bottom half on the back of flap i−1** (circularly), with the
  back-face half rotated 180° about the hinge axis so it reads upright when the
  flap has fallen. Glyphs are cut at the hinge line of the assembled pair, so
  the two halves meet with no offset across the gap.
- **Hinge convention (flap coordinates):** y = 0 is the hinge edge, the edge
  with the tabs, and the flap extends to y = H (≈31 mm). The **pivot axis** is
  the tab centre line, y = y_p (≈1.8 mm), and it lies on the display's
  horizontal midline. A raised flap (front visible) has its hinge at the
  bottom. A fallen flap (back visible) hangs with its hinge at the top. The
  glyph is placed in display coordinates centred on the midline, then split
  there: the part above the midline maps to the front of flap i, at
  y = y_p + d. The part below maps to the back of flap i−1 at y = y_p + d,
  mirrored so it reads upright on the fallen flap. Each flap's strip
  0 ≤ y < y_p is glyph-free, so the visible gap between halves is ~2·y_p plus
  the stack clearance. That is the same as the split look of the current
  display.
- **Inlay construction:** inlays are flush. Each colour body fills a pocket of
  the same shape cut into the black body, and the parts do not overlap. This
  matches the `FlapN_1/_2` parts in the archive. Flaps print flat with the
  **front face on the build plate**. The back-face inlay is the top surface.
  The default layer height is 0.2 mm, with 0.4 mm inlays (2 layers). The test
  plate adds a 0.3 mm inlay variant and a 1.2 mm-thick variant, to judge
  show-through against the black core.
- **Outline values are free to tune now.** Nothing mates with them yet. The
  drum (project 2) is built around whatever the test plate proves. The test
  plate's tab check is therefore "tabs print whole, are straight, and survive
  a bend by hand". Fit comes with the drum.
- **Colour flaps:** a colour flap is a black body with a **full-face inlay**
  in its colour. Its back face still carries the bottom half of the next flap
  in drum order, so the white flap's back is the blank's bottom (black). The
  ♥ is white, drawn as a parametric curve.
- **Plates by colour:** the 56 character flaps (black + white only) go on
  their own plates, so purge stays small. The 8 colour-bearing flaps share one
  plate per batch of drums, with the prime tower budgeted in its packing (bed
  margin and prime-tower area are parameters).
- **3mf:** aim for Bambu Studio's project metadata
  (`Metadata/model_settings.config`, extruder per part), so the parts open
  already assigned to AMS slots 1–8 (1 black, 2 white, 3–8 red…violet). Plain
  3mf with named parts is the fallback. Lucas verifies the import in Bambu
  Studio on his machine; the generator itself checks only that the 3mf is
  structurally valid.
- **Glyph size:** cap height is a parameter, 70 % of 2·H by default. The
  visible window from the frame is not known until project 2, so the test plate
  is the tuning loop.
- **Minimum feature:** 0.4 mm nozzle, so strokes ≥ 0.8 mm and dots ≥ 1.2 mm.
  One global stroke-offset parameter thickens thin glyphs. There are no
  per-glyph overrides.
- **Python:** `python3` 3.12 via uv. build123d pulls in OCP (large); it gets
  its **own pinned venv**, not the shared check venv, so other trees' checks do
  not get slower.
- Public repo: no LAN or personal data in files.

## The 64-flap set (drum order)

Following the Note (Vestaboard character codes), in code order:

| # | Content |
|---|---------|
| 0 | blank |
| 1–26 | A–Z |
| 27–36 | 1–9, 0 |
| 37–55 | `! @ # $ ( ) - + & = ; : ' " % , . / ?` |
| 56 | ♥ (drawn geometry, not a font glyph) |
| 57–63 | solid red, orange, yellow, green, blue, violet, white |

The list lives in one data file (e.g. `alphabet.toml` / `.json`) that the
generator reads; project 4 consumes the same list for the firmware alphabet, so
drum order and firmware cannot drift. A colour *character* shows its colour on
both visible halves (front of flap i, back of flap i−1), see "Colour flaps".

## Approaches

1. **Python + build123d generator** — *chosen.* Parametric, testable, lives in
   the repo next to our other Python tools, exports 3mf with separate parts.
   Costs about a day before the first print.
2. **OpenSCAD** — easy `text()`, but not installed, clumsy coloured-3mf export,
   and a second CAD language in the repo.
3. **Scale the existing STLs ~84 % in Bambu Studio** — a same-day printability
   probe, but cannot produce the 64-flap set or new glyphs; throwaway.

Lucas picked 1 (no spike first).

## Decided by Lucas

- Match the Note exactly (≈36 × 31 mm flap, 41.5 × 73 mm pitch).
- 64 flaps per drum including colours, Note's character set.
- New-board work reopened for project 3.
- Generator approach (build123d).
- Printer: X1C with two AMS.

## My calls (correctable)

- **Black flaps, white characters** (the Note's black model).
- **Font:** a bold condensed sans under the OFL (proposal: Oswald SemiBold or
  similar), the `.ttf` and its licence checked in. The cap height fills ~80 % of
  the 62 mm character height; final size tuned on the test plate.
- **Location:** `mechanics/flaps/` (new top-level dir): `generate.py`,
  `params.py` (all dimensions), `alphabet.*`, `fonts/`, `tests/`, a README with
  the print recipe. Output goes to `mechanics/flaps/out/` (gitignored); generated
  files are not committed.
- **Inlay:** 0.4 mm per face on a 1.0 mm flap (0.2 mm black core where front and
  back characters overlap); the test plate includes a 0.3 mm variant to compare
  show-through.
- **Test plate (~10 flaps):** `%`, `"`, `.`, `,`, `:`, `@`, ♥, `M`/`W` (wide
  strokes), `8`, one colour flap, plus a thickness/inlay variant pair.
- **Full set:** packed to the 256 mm bed with a margin, as many flaps per
  plate as fit (expect ~40, so 2 plates per drum), flat, character face down on
  the textured or smooth PEI as the README specifies.

## Left to the builder

- The exact outline curves, tab and clearance values within the constraints.
- 3mf structure details (one object per flap with per-colour parts, or one
  object per colour per plate) — whichever Bambu Studio imports as assignable
  parts; verify by importing one.
- Glyph-splitting implementation (2D boolean of the glyph against the half
  planes before extrusion).
- Tests: unit tests on the pure parts (alphabet load, front/back pairing rule,
  plate packing fits the bed, every inlay inside the flap outline, no part
  thinner than a nozzle width where avoidable); a smoke test that generates the
  test plate.
- Wiring a `mechanics` selection into `scripts/check.sh` with its own venv and
  pins, run only when `mechanics/` is touched or asked for by name.
