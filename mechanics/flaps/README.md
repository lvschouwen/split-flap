# Flap generator (Note-size, 64 flaps)

Generates the flaps for a Vestaboard-Note-footprint wall (3 x 15, ~41.5 mm
column / 73 mm row pitch) as Bambu-ready 3mf plates. Design:
`docs/superpowers/specs/2026-11-20-note-size-flaps-design.md` (#670).

## Run

```bash
uv venv --python 3.12 ~/.cache/split-flap/mech-venv
uv pip install --python ~/.cache/split-flap/mech-venv/bin/python build123d==0.13.0 pytest==9.2.0
cd mechanics
~/.cache/split-flap/mech-venv/bin/python -m flaps.generate --test-plate      # out/test-plate.3mf
~/.cache/split-flap/mech-venv/bin/python -m flaps.generate --full            # out/drum-plate-{1,2,3}.3mf
~/.cache/split-flap/mech-venv/bin/python -m flaps.generate --test-plate --set cap_frac=0.65 --set inlay_depth=0.3
```

`scripts/check.sh mech` builds the venv itself and runs the tests. Output in
`out/` is never committed. `--plain-3mf` drops the Bambu metadata (assign
filaments by hand if Bambu Studio rejects the project file).

## What is where

| File | Holds |
|------|-------|
| `params.py` | every dimension (mm); validated, so an unprintable override fails |
| `alphabet.json` | the drum order, 64 entries; the firmware alphabet will read it too |
| `layout.py` | which half goes where: char i top = front of flap i, bottom = back of flap i-1 |
| `geometry.py` | flap outline, glyph halves, flush inlay pockets |
| `plates.py` | grid packing on the 256 mm bed, prime-tower keep-out |
| `threemf.py` | 3mf with one part per colour and its AMS slot |

## AMS slots (X1C, two AMS)

| Slot | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 |
|------|---|---|---|---|---|---|---|---|
| Filament | black | white | red | orange | yellow | green | blue | violet |

## Print recipe

- 0.4 mm nozzle, 0.2 mm layers; front face down on the plate (smooth or textured PEI).
- Plain plates (`drum-plate-1`, `-2`): black + white only, no prime tower needed.
- Colour plate (`drum-plate-3`) and the test plate: enable the prime tower and
  drag it into the top-right corner, at most 35 x 35 mm (`prime_tower`); the
  packing keeps that square plus `spacing` free. Bambu Studio picks its own
  default spot, so check it before slicing.
- The 3mf says it comes from Bambu Studio 2.0 so per-part slots load; an older
  Studio may warn about the version. `--plain-3mf` avoids that.
- `cap_frac` 0.70 is the largest cap height at which no Barlow glyph is clipped;
  a clipped glyph or a failing `stroke_offset` stops the run with its name.
- Test plate: 11 stress flaps (`% " . , : @ M W 8`, heart, red) plus `A`
  at 0.3 mm inlay (`flap01_inlay03`) and 1.2 mm thick (`flap01_t12`).
  Judge legibility, white show-through, tab strength (bend by hand).

## Tuning loop

Change a value with `--set KEY=VALUE` (any field of `FlapParams`), reprint
the test plate, and once settled make it the default in `params.py`.
Glyphs wider than the narrow zone (`@ % W # &` in Barlow Condensed) are
squeezed horizontally; lowering `cap_frac` reduces that.
