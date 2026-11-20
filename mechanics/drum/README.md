# Drum and unit frame generator (Note-size, 64 flaps)

Generates the drum halves, motor adapter sleeve, frame walls, bearing ring, display
plate, stop lip and back plate for one unit of the Note wall as 3mf plates (one colour,
3MF core only). Design: `docs/superpowers/specs/2026-11-20-note-drum-frame-design.md`
(#673). Flap dimensions are read from `flaps/params.py` (`DrumParams.flap`), never copied.

## Run

Same venv as `mechanics/flaps` (see its README), from `mechanics/`:

```bash
~/.cache/split-flap/mech-venv/bin/python -m drum.generate --coupon          # out/coupon-1.3mf: hole_slop 0.0-0.4
~/.cache/split-flap/mech-venv/bin/python -m drum.generate --test-assembly   # out/test-assembly-N.3mf: hand-turn parts
~/.cache/split-flap/mech-venv/bin/python -m drum.generate --full            # + the motor adapter sleeve
~/.cache/split-flap/mech-venv/bin/python -m drum.generate --assembled       # out/assembled.3mf, every part in place
~/.cache/split-flap/mech-venv/bin/python -m drum.generate --test-assembly --set hole_slop=0.1
```

`scripts/check.sh mech` runs both generators' tests. `out/` is never committed.
`DrumParams` refuses an infeasible combination (hole web, row height, depth stack, encoder
boss) with the reason.

## What is where

| File | Holds |
|------|-------|
| `params.py` | every dimension (mm), derived values, validation through `feasibility.check` |
| `feasibility.py` | pure arithmetic: pivot-radius window, release overlap, stop edge, pack clearance, depth stack, hub stack-up |
| `layout.py` | pure shelf packing on the 256 mm bed |
| `cad.py` | build123d helpers |
| `drum.py` | half A (motor), half B (far, axle boss + magnet pocket), adapter sleeve |
| `frame.py` | side walls (far wall: post, FFC slot and groove), bearing ring, rails, display plate, stop lip, back plate |
| `assembly.py` | the parts, their print frames and assembly poses |
| `threemf.py` | writes parts via the flap writer's 3MF helpers |

## Print notes

- Drum halves: disc face down, hub up; M2 screws enter through the access holes in the far disc.
- Walls and rails: inside face up; the bottom rail is the flat floor guide.
- Stop lip: printed on its face, the lower edge S on the bed.
- Not generated yet: motor clamp pattern, board mounting bosses (#674 pattern), clips between frames.
