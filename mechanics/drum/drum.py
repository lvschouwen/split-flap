"""Drum halves and motor sleeve (#673 section 2, 7). Print frame: disc outer face on z = 0, hub up.

Half A is the motor side (bulkhead with the D coupling, pilot holes for the M2 screws),
half B the far side (axle boss with the magnet pocket, screw clearance holes and
access holes through its disc). The M2 screws enter from the far disc.
"""
from dataclasses import dataclass

from drum.cad import box, cyl, polar, rot_z, tube
from drum.params import DrumParams


@dataclass(frozen=True)
class Halves:
    a: object
    b: object


def _disc(p: DrumParams, hub_h: float):
    """Disc with the 64 pivot holes and a hub tube (solid inside, bored by the caller)."""
    disc = cyl(p.r_outer, p.disc_t)
    for i in range(p.count):
        x, y = polar(p.r_p, 360.0 * i / p.count)
        disc = disc - cyl(p.hole_d / 2, p.disc_t + 2, -1, x, y)
    return disc + cyl(p.r_hub, hub_h)


def _lug_angles(p: DrumParams) -> list[float]:
    return [90.0 + 360.0 * k / p.lug_count for k in range(p.lug_count)]


def lug_xy(p: DrumParams, deg: float) -> tuple[float, float]:
    return polar(p.r_bore - p.lug_w / 2, deg)


def _lugs(p: DrumParams, z0: float):
    out = None
    for a in _lug_angles(p):
        lug = rot_z(box(p.r_bore - p.lug_w, p.r_bore + 0.3, -p.lug_w / 2, p.lug_w / 2, z0, z0 + p.lug_t), a)
        out = lug if out is None else out + lug
    return out


def _d_hole(p: DrumParams, z0: float, h: float):
    d = cyl((p.shaft_d + 0.2) / 2, h, z0)
    return d & box(-p.shaft_d, p.shaft_d, -(p.shaft_flat + 0.2) / 2, (p.shaft_flat + 0.2) / 2, z0, z0 + h)


def half_a(p: DrumParams):
    """Motor-side half: disc with the motor bore, bulkhead + D coupling, lug layer, spigot cut."""
    u_j = p.x_joint + p.x_out
    u_bulk = p.disc_t + p.motor_len
    body = _disc(p, u_j) - cyl(p.r_bore, u_j + 2, -1)
    body = body + cyl(p.r_hub, p.bulkhead_t, u_bulk) - _d_hole(p, u_bulk - 1, p.bulkhead_t + 2)
    lugs = _lugs(p, u_j - p.lug_t)
    for a in _lug_angles(p):
        x, y = lug_xy(p, a)
        lugs = lugs - cyl(p.screw_pilot_d / 2, p.lug_t, u_j - p.lug_t, x, y)
    r_mid = p.r_bore + p.wall / 2 - p.spigot_clear
    cut = cyl(p.r_hub + 1, p.spigot_len + 1, u_j - p.spigot_len) - cyl(r_mid, p.spigot_len + 3, u_j - p.spigot_len - 1)
    return (body + lugs) - cut


def half_b(p: DrumParams):
    """Far-side half: disc, hub, spigot lip, lug layer with clearance holes, axle boss."""
    u_j = p.x_out - p.x_joint
    body = _disc(p, u_j) - cyl(p.r_bore, u_j + p.spigot_len + 2, p.disc_t)
    r_mid = p.r_bore + p.wall / 2
    body = body + tube(p.r_hub, r_mid, p.spigot_len, u_j)
    lugs = _lugs(p, u_j - p.lug_t)
    head_r = p.screw_clear_d / 2 + 0.9
    for a in _lug_angles(p):
        x, y = lug_xy(p, a)
        lugs = lugs - cyl(p.screw_clear_d / 2, p.lug_t + 2, u_j - p.lug_t - 1, x, y)
        body = body - cyl(head_r, p.disc_t + 2, -1, x, y)
    boss = cyl(p.boss_od / 2, p.boss_len)
    bore_h = p.magnet_x_from_wall - p.side_gap
    out = body + lugs + boss - cyl(p.boss_bore_d / 2, bore_h + 1, -1)
    return out - cyl((p.magnet_d + p.magnet_slop) / 2, p.magnet_t, bore_h)


def build_halves(p: DrumParams) -> Halves:
    return Halves(half_a(p), half_b(p))


def sleeve(p: DrumParams, inner_d: float, length: float):
    """Motor adapter: slides into the hub bore, holds a smaller motor (inner_d)."""
    return tube(p.r_bore - 0.1, inner_d / 2, length)
