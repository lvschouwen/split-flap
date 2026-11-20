import math

import pytest

from drum import feasibility as fe
from drum.params import DrumParams


def test_chord_matches_spec():
    assert fe.chord(64, 18.5) == pytest.approx(1.816, abs=1e-3)


def test_pivot_window_is_18_3_to_19_8():
    p = DrumParams()
    assert fe.min_pivot_radius(64, p.hole_d, 0.6) == pytest.approx(18.34, abs=0.01)
    assert fe.max_pivot_radius(p.row_inner, p.pivot_to_edge) == pytest.approx(19.8, abs=0.05)
    assert fe.min_pivot_radius(64, p.hole_d, 0.6) <= p.r_p <= fe.max_pivot_radius(p.row_inner, p.pivot_to_edge)


def test_hole_web_and_outer_radius():
    p = DrumParams()
    assert fe.hole_web(64, p.r_p, p.hole_d) == pytest.approx(0.616, abs=1e-3)
    assert p.r_outer == pytest.approx(19.7)


def test_release_overlap_inside_one_chord():
    c = fe.chord(64, 18.5)
    o = fe.release_overlap(64, 18.5)
    assert 0 < o < c
    assert o == pytest.approx(0.908, abs=1e-3)


def test_stop_edge_matches_spec():
    p = DrumParams()
    assert fe.stop_height(p.r_p, 64, p.pivot_to_edge) == pytest.approx(30.1, abs=0.05)
    assert fe.stop_z(p.r_p, 64, p.stop_dz) == pytest.approx(-0.91, abs=0.01)


def test_lower_pack_clearance_and_ceiling():
    p = DrumParams()
    c = fe.pack_clearance(64, p.r_p, p.row_inner, p.pivot_to_edge, p.flap.thickness)
    assert c == pytest.approx(0.245, abs=0.01)
    best = max(fe.chord(64, r) * ((p.row_inner / 2 - r) / p.pivot_to_edge) for r in [x / 10 for x in range(150, 199)])
    assert best == pytest.approx(1.047, abs=0.003)


def test_depth_stack_closes_in_the_case():
    p = DrumParams()
    total = fe.depth_total(p)
    assert total == pytest.approx(99.3, abs=0.1)
    assert p.depth_budget - total >= 1.5
    assert p.depth_budget - total - 1.0 > 0  # room for a 1.0 mm case panel
    assert p.z_back == pytest.approx(92.5, abs=0.1)


def test_old_depth_stack_did_not_close():
    with pytest.raises(ValueError, match="depth stack"):
        DrumParams(tip_clear=1.0, sheet_t=1.0, standoff=3.1, socket_sunk=0.0)


def test_encoder_stack_closes_in_the_hub():
    p = DrumParams()
    assert fe.hub_free_length(p) == pytest.approx(12.8)
    assert fe.boss_projection(p) == pytest.approx(11.25)
    assert fe.hub_free_length(p) - fe.boss_projection(p) == pytest.approx(1.55)
    assert p.boss_len == pytest.approx(13.25)


def test_frame_stays_inside_the_column_pitch():
    p = DrumParams()
    assert p.frame_w == pytest.approx(40.2)
    assert p.col_pitch - p.frame_w == pytest.approx(1.3)


def test_joint_plane():
    p = DrumParams()
    assert p.x_joint == pytest.approx(7.7)
    assert p.x_joint > -p.x_in + p.motor_len + p.bulkhead_t


@pytest.mark.parametrize("kw,msg", [
    ({"r_p": 17.0}, "min_web"),
    ({"r_p": 21.0}, "row"),
    ({"hole_slop": 0.8}, "min_web"),
    ({"motor_d": 34.0}, "bore"),
    ({"magnet_t": 5.0}, "boss"),
])
def test_infeasible_combinations_name_the_problem(kw, msg):
    with pytest.raises(ValueError, match=msg):
        DrumParams(**kw)
