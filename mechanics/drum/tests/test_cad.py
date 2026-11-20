"""Geometry of the drum and frame parts, in print frame and in place."""
import math

import pytest

from drum import assembly as asm
from drum import drum as dr
from drum import frame as fr
from drum.params import DrumParams

P = DrumParams()


@pytest.fixture(scope="module")
def parts():
    return {q.name: q for q in asm.parts(P)}


def test_halves_hold_64_pivot_holes_on_the_circle():
    half = dr.half_b(P)
    assert half.is_valid
    # a through hole on the pitch circle: probing a thin rod at each angle finds no material
    from drum.cad import cyl, polar
    for i in (0, 1, 17, 63):
        x, y = polar(P.r_p, 360.0 * i / 64)
        rod = cyl(P.flap.pin_d / 2, P.disc_t, 0, x, y)
        assert (half & rod).volume == pytest.approx(0.0, abs=1e-6)
    # and between two holes there is material
    x, y = polar(P.r_p, 360.0 * 0.5 / 64)
    assert (half & cyl(0.1, P.disc_t, 0, x, y)).volume > 0.05


def test_hole_count_by_volume():
    from drum.cad import cyl
    solid_disc = cyl(P.r_outer, P.disc_t)
    half = dr.half_b(P)
    ring = (half & cyl(P.r_outer, P.disc_t)) - cyl(P.r_hub, P.disc_t)
    drilled = (solid_disc - cyl(P.r_hub, P.disc_t)).volume - ring.volume
    # 64 holes plus the 3 screw access holes lie inside r_hub only partly: subtract them
    expect = 64 * math.pi * (P.hole_d / 2) ** 2 * P.disc_t
    assert drilled == pytest.approx(expect, rel=0.01)


def test_part_extents(parts):
    a, b = parts["drum-a"].solid.bounding_box(), parts["drum-b"].solid.bounding_box()
    assert a.size.Z == pytest.approx(P.x_joint + P.x_out)
    assert b.size.Z == pytest.approx(P.boss_len)
    assert a.size.X == pytest.approx(2 * P.r_outer)


def test_assembly_inside_the_envelope(parts):
    boxes = {n: s.bounding_box() for n, s in asm.assembled(P)}
    xs = [b.max.X for b in boxes.values()] + [-b.min.X for b in boxes.values()]
    assert max(xs) <= P.frame_w / 2 + 1e-6
    assert max(b.max.Y for b in boxes.values()) <= P.row_pitch / 2 + 1e-6
    assert min(b.min.Y for b in boxes.values()) >= -P.row_pitch / 2 - 1e-6
    assert max(b.max.Z for b in boxes.values()) <= P.z_back + P.back_wall + 1e-6
    # the bearing ring (outer radius r_outer + ring_clear/2 + wall, axis at z = r_p) and the lip ledge stand
    # in front of the display plane; nothing else does
    front = min(b.min.Z for b in boxes.values())
    assert front == pytest.approx(P.r_p - (P.r_outer + P.ring_clear / 2 + P.wall))
    assert boxes["front-plate"].min.Z == pytest.approx(-P.front_wall)


def test_stop_lip_sits_at_the_stop_edge(parts):
    s_y, z_s = fr.stop_edge(P)
    b = dict(asm.assembled(P))["stop-lip"].bounding_box()
    assert b.min.Y == pytest.approx(s_y, abs=1e-6)
    assert b.max.Z == pytest.approx(z_s, abs=1e-6)


PAIRS = [("drum-a", "drum-b"), ("drum-a", "bearing-ring"), ("drum-b", "wall-far"), ("drum-a", "wall-motor"),
         ("drum-b", "rail-bottom"), ("drum-a", "rail-top"), ("drum-a", "front-plate"), ("drum-b", "back-plate"),
         ("front-plate", "stop-lip"), ("stop-lip", "rail-top"), ("wall-motor", "bearing-ring"),
         ("wall-far", "rail-bottom"), ("wall-motor", "back-plate"), ("front-plate", "wall-far"),
         ("drum-b", "sleeve"), ("drum-a", "sleeve"), ("bearing-ring", "front-plate"), ("drum-b", "front-plate"),
         ("bearing-ring", "drum-a"), ("wall-far", "front-plate"), ("wall-motor", "rail-top")]


@pytest.mark.parametrize("a,b", PAIRS)
def test_parts_do_not_intersect(a, b):
    placed = dict(asm.assembled(P))
    assert (placed[a] & placed[b]).volume == pytest.approx(0.0, abs=1e-3)


def test_post_enters_the_boss_bore_without_touching():
    placed = dict(asm.assembled(P))
    assert (placed["wall-far"] & placed["drum-b"]).volume == pytest.approx(0.0, abs=1e-3)
    post = placed["wall-far"].bounding_box()
    boss = placed["drum-b"].bounding_box()
    assert post.min.X < boss.max.X  # the post reaches into the boss


def test_groove_inside_the_wall():
    wall = fr.side_wall(P, True)
    assert wall.is_valid
    assert P.wall - P.groove_depth >= 0.6  # section 7: a 0.6 mm skin stays


def test_magnet_pocket_holds_the_magnet():
    half = dr.half_b(P)
    from drum.cad import cyl
    bore_h = P.magnet_x_from_wall - P.side_gap
    assert (half & cyl(P.magnet_d / 2, P.magnet_t, bore_h)).volume == pytest.approx(0.0, abs=1e-3)
    assert (half & cyl(P.boss_bore_d / 2 - 0.1, bore_h, 0)).volume == pytest.approx(0.0, abs=1e-3)
