import pytest

from flaps.alphabet import Entry, load_alphabet
from flaps.geometry import build_flap, flap_outline, glyph_display, half_on_flap
from flaps.params import FlapParams

P = FlapParams()
A = load_alphabet()
EPS = 1e-3


def _bb(shape):
    return shape.bounding_box()


def test_outline_bbox():
    bb = _bb(flap_outline(P))
    assert bb.size.X == pytest.approx(36.0) and bb.size.Y == pytest.approx(31.0)
    assert bb.min.Y == pytest.approx(0.0)


def test_outline_narrow_zone_and_tabs():
    o = flap_outline(P)
    def width_at(y):
        from build123d import Rectangle, Pos
        strip = o & (Pos(0, y) * Rectangle(100, 0.01))
        return _bb(strip).size.X
    assert width_at(8.0) == pytest.approx(32.5, abs=1e-3)
    assert width_at(1.8) == pytest.approx(36.0, abs=1e-3)   # the tabs
    assert width_at(0.5) == pytest.approx(32.5, abs=1e-3)
    assert width_at(20.0) == pytest.approx(36.0, abs=1e-3)


@pytest.mark.parametrize("i", [0, 1, 56, 60])
def test_volume_is_conserved(i):
    parts = build_flap(i, A, P)
    total = parts.body.volume + sum(p.volume for p in parts.inlays.values())
    assert total == pytest.approx(flap_outline(P).area * P.thickness, rel=1e-4)
    for part in parts.inlays.values():
        assert _bb(part).min.Z >= -EPS and _bb(part).max.Z <= P.thickness + EPS


def test_inlays_inside_outline():
    p = P.with_overrides(cap_frac=0.80)
    o = _bb(flap_outline(p))
    for e in A:
        for half in ("top", "bottom"):
            s = half_on_flap(e, half, p)
            if s is None:
                continue
            bb = _bb(s)
            assert bb.min.X >= o.min.X - EPS and bb.max.X <= o.max.X + EPS, (e, half)
            assert bb.min.Y >= p.pivot_y - EPS and bb.max.Y <= o.max.Y + EPS, (e, half)


def test_wide_glyphs_fit_the_narrow_zone():
    for c in "@%W#&":
        bb = _bb(glyph_display(Entry("char", c), P))
        assert bb.size.X <= P.narrow_width - 2 * P.edge_margin + EPS, c


def test_glyphs_share_a_baseline():
    h, e = _bb(glyph_display(Entry("char", "H"), P)), _bb(glyph_display(Entry("char", "E"), P))
    assert h.min.Y == pytest.approx(e.min.Y, abs=0.05)
    assert (h.max.Y + h.min.Y) / 2 == pytest.approx(0.0, abs=0.05)
    assert h.size.Y == pytest.approx(P.cap_frac * P.char_height, rel=1e-3)


def test_blank_flap_front_has_no_inlay():
    parts = build_flap(0, A, P)
    assert set(parts.inlays) == {"white"}
    assert _bb(parts.inlays["white"]).min.Z == pytest.approx(P.thickness - P.inlay_depth)


def test_colour_flap_parts():
    assert set(build_flap(57, A, P).inlays) == {"red", "orange"}
    assert set(build_flap(63, A, P).inlays) == {"white"}


def test_front_inlay_sits_on_the_plate():
    red = build_flap(57, A, P).inlays["red"]
    assert _bb(red).min.Z == pytest.approx(0.0) and _bb(red).max.Z == pytest.approx(P.inlay_depth)


def test_asymmetric_glyph_halves():
    # 'L': the stem is left; the foot runs right. x is never mirrored on the back.
    L = Entry("char", "L")
    top, bottom = _bb(half_on_flap(L, "top", P)), _bb(half_on_flap(L, "bottom", P))
    assert (top.min.X + top.max.X) / 2 < 0
    assert bottom.max.X > top.max.X + 5


def test_heart_is_white():
    assert set(build_flap(56, A, P).inlays) == {"white", "red"}


def test_stroke_offset_grows_glyphs():
    thin = glyph_display(Entry("char", "I"), P).area
    thick = glyph_display(Entry("char", "I"), P.with_overrides(stroke_offset=0.2)).area
    assert thick > thin
