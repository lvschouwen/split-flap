import math

import pytest

from flaps.alphabet import Entry, load_alphabet
from flaps.geometry import build_flap, flap_outline, glyph_display, half_on_flap, pivot_pins
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


def test_outline_narrow_zone_and_wide_top():
    o = flap_outline(P)
    def width_at(y):
        from build123d import Rectangle, Pos
        strip = o & (Pos(0, y) * Rectangle(100, 0.01))
        return _bb(strip).size.X
    assert width_at(8.0) == pytest.approx(32.5, abs=1e-3)
    assert width_at(1.8) == pytest.approx(32.5, abs=1e-3)   # the pins are not in the outline
    assert width_at(0.5) == pytest.approx(32.5, abs=1e-3)
    assert width_at(20.0) == pytest.approx(36.0, abs=1e-3)


@pytest.mark.parametrize("i", [0, 1, 56, 60])
def test_volume_is_conserved(i):
    parts = build_flap(i, A, P)
    total = parts.body.volume + sum(p.volume for p in parts.inlays.values())
    pins = sum(pin.volume for pin in pivot_pins(P))
    assert total == pytest.approx(flap_outline(P).area * P.thickness + pins, rel=1e-4)
    for part in parts.inlays.values():
        assert _bb(part).min.Z >= -EPS and _bb(part).max.Z <= P.thickness + EPS


def test_inlays_inside_outline():
    # 0.70 is the largest default-font cap height with no glyph clipped (0.75 clips Q $ ( ) ; ,)
    p = P.with_overrides(cap_frac=0.70)
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
    # 'L': stem on the display's left, foot running right. Both faces are read
    # from -z, so display-left is flap +x on the front and on the fallen back.
    L = Entry("char", "L")
    top, bottom = _bb(half_on_flap(L, "top", P)), _bb(half_on_flap(L, "bottom", P))
    assert (top.min.X + top.max.X) / 2 > 0
    assert bottom.min.X < top.min.X - 5


def test_clipped_glyph_fails_loudly():
    with pytest.raises(ValueError, match="clipped"):
        half_on_flap(Entry("char", "Q"), "bottom", P.with_overrides(cap_frac=0.9))


def test_heart_is_white():
    assert set(build_flap(56, A, P).inlays) == {"white", "red"}


def test_failed_stroke_offset_names_the_glyph():
    with pytest.raises(ValueError, match="stroke_offset"):
        for e in A:
            glyph_display(e, P.with_overrides(stroke_offset=0.1))


def test_stroke_offset_grows_glyphs():
    thin = glyph_display(Entry("char", "I"), P).area
    thick = glyph_display(Entry("char", "I"), P.with_overrides(stroke_offset=0.2)).area
    assert thick > thin


def test_pin_envelope_and_flats():
    left, right = pivot_pins(P)
    for pin in (left, right):
        bb = _bb(pin)
        assert bb.min.Z == pytest.approx(0.0, abs=EPS) and bb.max.Z == pytest.approx(P.thickness, abs=EPS)
        assert bb.size.Y == pytest.approx(P.pin_d, abs=EPS)
        assert bb.size.X == pytest.approx(P.tab_reach, abs=EPS)
        assert (bb.min.Y + bb.max.Y) / 2 == pytest.approx(P.pivot_y, abs=EPS)
        for v in pin.vertices():
            r = math.hypot(v.Y - P.pivot_y, v.Z - P.thickness / 2)
            assert r <= P.pin_d / 2 + P.tess_tol
    assert _bb(left).min.X == pytest.approx(-P.width / 2, abs=EPS)
    assert _bb(right).max.X == pytest.approx(P.width / 2, abs=EPS)


def test_pin_cross_section_is_a_truncated_circle():
    r, h = P.pin_d / 2, P.thickness / 2
    seg = 2 * (math.asin(h / r) * r * r + h * math.sqrt(r * r - h * h))  # circle clipped to |z| <= h
    assert pivot_pins(P)[0].volume == pytest.approx(seg * P.tab_reach, rel=1e-3)


def test_pin_joins_the_body():
    body = build_flap(1, A, P).body
    assert len(body.solids()) == 1
    assert _bb(body).size.X == pytest.approx(P.width, abs=EPS)
