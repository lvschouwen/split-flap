import pytest

from flaps.params import FlapParams


def test_defaults_match_spec():
    p = FlapParams()
    assert (p.width, p.height, p.thickness, p.inlay_depth) == (36.0, 31.0, 0.8, 0.2)
    assert p.tab_reach == 1.75
    assert (p.pin_d, p.pivot_y) == (1.0, pytest.approx(1.8))
    assert p.char_height == 62.0


@pytest.mark.parametrize("kw", [
    {"inlay_depth": 0.4},      # inlay must stay below half the 0.8 thickness
    {"narrow_width": 34.0},    # tab reach 1.0 < 1.5
    {"cap_frac": 0.0},
    {"cap_frac": 0.95},
    {"pin_d": 0.9},            # pin below 1.0
    {"thickness": 1.2},        # thicker than the pin: no flats
    {"pivot_y": 0.5},          # pin runs through the hinge edge
    {"pivot_y": 16.0},         # pin leaves the narrow zone
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
