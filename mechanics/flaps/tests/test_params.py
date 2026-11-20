import pytest

from flaps.params import FlapParams


def test_defaults_match_spec():
    p = FlapParams()
    assert (p.width, p.height, p.thickness, p.inlay_depth) == (36.0, 31.0, 1.0, 0.4)
    assert p.tab_reach == 1.75
    assert p.pivot_y == pytest.approx(1.8)
    assert p.char_height == 62.0


@pytest.mark.parametrize("kw", [
    {"inlay_depth": 0.5},      # inlay must stay below half the thickness
    {"narrow_width": 34.0},    # tab reach 1.0 < 1.5
    {"cap_frac": 0.0},
    {"cap_frac": 0.95},
    {"tab_h": 0.8},            # tab height below 1.0
    {"edge_margin": -0.5},     # would outset the inlay past the outline
    {"edge_margin": 16.5},     # collapses the inlay region
    {"spacing": -1.0},
    {"bed_margin": -1.0},
    {"prime_tower": 0.0},
    {"tess_tol": 0.0},
    {"thickness": float("nan")},
])
def test_params_reject_unprintable(kw):
    with pytest.raises(ValueError):
        FlapParams().with_overrides(**kw)


def test_overrides_return_a_new_value():
    p = FlapParams()
    q = p.with_overrides(cap_frac=0.75)
    assert q.cap_frac == 0.75 and p.cap_frac == 0.70


def test_unknown_override_is_rejected():
    with pytest.raises(ValueError, match="nope"):
        FlapParams().with_overrides(nope=1.0)
