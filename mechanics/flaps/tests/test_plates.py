import itertools

from flaps.params import FlapParams
from flaps.plates import Placed, grid, pack


def _rect(pl, p):
    return pl.x, pl.y, pl.x + p.width, pl.y + p.height


def _gap(a, b):
    dx = max(b[0] - a[2], a[0] - b[2], 0)
    dy = max(b[1] - a[3], a[1] - b[3], 0)
    return max(dx, dy)


def test_pack_respects_bed_tower_and_spacing():
    p = FlapParams().with_overrides(width=40.0, narrow_width=36.5)
    flaps = list(range(64))
    plates = pack(flaps, p, tower=True)
    lo, hi = p.bed_margin, p.bed - p.bed_margin
    tower = (hi - p.prime_tower, hi - p.prime_tower, hi, hi)
    seen = []
    for plate in plates:
        rects = [_rect(pl, p) for pl in plate]
        for r in rects:
            assert lo - 1e-9 <= r[0] and r[2] <= hi + 1e-9 and lo - 1e-9 <= r[1] and r[3] <= hi + 1e-9
            assert _gap(r, tower) >= p.spacing - 1e-9
        for a, b in itertools.combinations(rects, 2):
            assert _gap(a, b) >= p.spacing - 1e-9
        seen += [pl.flap for pl in plate]
    assert sorted(seen) == flaps


def test_default_plate_counts():
    # cols = (256-2*5+3)//(36+3) = 6, rows = (256-2*5+3)//(31+3) = 7 -> 42 per plate
    assert [len(pl) for pl in pack(list(range(64)), FlapParams(), tower=False)] == [42, 22]


def test_grid_keeps_bed_width_and_spacing_but_grows_in_y():
    p = FlapParams()
    placed = grid(list(range(64)), p)
    assert [pl.flap for pl in placed] == list(range(64))
    rects = [_rect(pl, p) for pl in placed]
    assert all(p.bed_margin - 1e-9 <= r[0] and r[2] <= p.bed - p.bed_margin + 1e-9 for r in rects)
    assert max(r[3] for r in rects) > p.bed
    for a, b in itertools.combinations(rects, 2):
        assert _gap(a, b) >= p.spacing - 1e-9


def test_placed_is_a_value():
    assert Placed(3, 1.0, 2.0) == Placed(3, 1.0, 2.0)
