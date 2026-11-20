import dataclasses

import pytest

from drum.params import DrumParams
from flaps.params import FlapParams


def test_flap_dimensions_come_from_flaps_params():
    p = DrumParams()
    f = FlapParams()
    assert (p.flap.thickness, p.flap.width, p.flap.height) == (f.thickness, f.width, f.height)
    assert p.pivot_to_edge == f.height - f.pivot_y
    assert p.hole_d == pytest.approx(f.pin_d + p.hole_slop)
    names = {x.name for x in dataclasses.fields(DrumParams)}
    assert not names & {"thickness", "width", "height", "pin_d", "pivot_y", "narrow_width"}


def test_flap_override_flows_into_the_drum():
    p = DrumParams(flap=FlapParams(pivot_y=2.0))
    assert p.pivot_to_edge == pytest.approx(29.0)


def test_with_overrides_and_unknown_key():
    assert DrumParams().with_overrides(hole_slop=0.1).hole_d == pytest.approx(1.1)
    with pytest.raises(ValueError, match="unknown"):
        DrumParams().with_overrides(bogus=1.0)


def test_non_finite_rejected():
    with pytest.raises(ValueError, match="finite"):
        DrumParams(g=float("nan"))
