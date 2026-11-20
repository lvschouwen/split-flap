import itertools

from flaps.alphabet import load_alphabet
from flaps.params import FlapParams
from flaps.plates import Placed, pack, split_by_colour


def test_split_by_colour():
    plain, coloured = split_by_colour(load_alphabet())
    # 63 (solid white, blank back) needs only black + white
    assert coloured == list(range(56, 63))
    assert plain == list(range(56)) + [63]


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
    plain, coloured = split_by_colour(load_alphabet())
    # cols = (256-2*5+3)//(36+3) = 6, rows = (256-2*5+3)//(31+3) = 7 -> 42 per plate
    assert [len(pl) for pl in pack(plain, FlapParams(), tower=False)] == [42, 15]
    assert len(pack(coloured, FlapParams(), tower=True)) == 1


def test_placed_is_a_value():
    assert Placed(3, 1.0, 2.0) == Placed(3, 1.0, 2.0)
