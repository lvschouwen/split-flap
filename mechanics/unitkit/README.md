# unitkit

One 3mf with every printed part of a unit (#678): all 64 flaps (multi-colour, one part per
colour, as `flaps.generate --full`) and every part of `drum.generate --full`, each in print
orientation on one grid. The grid runs past the 256 mm bed in y; Studio's Arrange spreads it.
Geometry comes from [`flaps/`](../flaps/README.md) and [`drum/`](../drum/README.md) unchanged.

```
cd mechanics && python -m unitkit --out out/unit.3mf     # add --tess-tol 0.2 for a quick draft
```

Objects: `flap00`..`flap63`, `drum_half_a`, `drum_half_b`, `sleeve`, `wall_motor`, `wall_far`,
`bearing_ring`, `rail_bottom`, `rail_top`, `display_plate`, `stop_lip`, `back_plate`.

## Studio 2.9

1. Open `out/unit.3mf`; answer the colour dialog by mapping the colours to AMS slots:
   1 black, 2 white, 3 red, 4 orange, 5 yellow, 6 green, 7 blue, 8 violet.
2. Arrange (A) to spread the objects over plates.
3. Enable the prime tower on the plates that carry coloured flaps.

Drum and frame parts are black only.

## Not generated yet

Motor clamp, board mounting bosses, clips between frames.

Tests: `python -m pytest -q tests/` here (`scripts/check.sh mech`).
