"""Unit frame parts (#673 sections 3-6). Each part is built in its print frame (z up from the bed);
assembly.py places it. Side walls and rails lie flat (x = distance along the unit's z, y = row y);
walls have the inside face up. Frame coordinates for the assembly are in params.py's docstring."""
from build123d import Cylinder, Pos, Rot

from drum import feasibility as fe
from drum.cad import box, cyl, tube
from drum.params import DrumParams


def stop_edge(p: DrumParams) -> tuple[float, float]:
    """(height above the midline, z) of the stop edge S."""
    n = p.count
    return (fe.stop_height(p.r_p, n, p.pivot_to_edge), fe.stop_z(p.r_p, n, p.stop_dz))


def z_extent(p: DrumParams) -> tuple[float, float]:
    return (-p.front_wall, p.z_back + p.back_wall)


def side_wall(p: DrumParams, far: bool):
    """Side wall, 73 x depth, inside face up. Far wall: post + FFC slot + groove."""
    z0, z1 = z_extent(p)
    h = p.row_pitch / 2
    plate = box(z0, z1, -h, h, 0, p.wall)
    if not far:
        return plate
    ax = p.r_p
    post = cyl(p.post_d / 2, p.post_len, p.wall, ax, 0)
    slot = box(ax - p.ffc_slot_t / 2, ax + p.ffc_slot_t / 2, -p.ffc_slot_w / 2, p.ffc_slot_w / 2,
               p.wall - 0.01, p.wall + p.post_len + 1)
    groove = box(ax, z1 + 1, -p.groove_w / 2, p.groove_w / 2, p.wall - p.groove_depth, p.wall + 0.01)
    return (plate - groove) + post - slot


def bearing_ring(p: DrumParams):
    """Plain bearing ring for the motor-side wall: bore = disc rim + ring_clear."""
    r_in = p.r_outer + p.ring_clear / 2
    return tube(r_in + p.wall, r_in, p.ring_len)


def rail(p: DrumParams):
    """Top or bottom wall (the bottom one is the flat floor guide), spans the frame between the side walls."""
    z0, z1 = z_extent(p)
    return box(z0, z1, -p.x_wall, p.x_wall, 0, p.wall)


def front_plate(p: DrumParams):
    """Display plate with the window and the notch the stop lip bridges."""
    w, h = 2 * p.x_wall, p.row_inner
    plate = box(-w / 2, w / 2, -h / 2, h / 2, 0, p.front_wall)
    plate = plate - box(-p.window_w / 2, p.window_w / 2, -p.window_h / 2, p.window_h / 2, -1, p.front_wall + 1)
    s_y, _ = stop_edge(p)
    plate = plate - _rim_relief(p)
    return plate - box(-p.lip_w / 2, p.lip_w / 2, p.window_h / 2 - 0.01, s_y + p.lip_t, -1, p.front_wall + 1)


def _rim_relief(p: DrumParams):
    """The disc rims and the bearing rings reach in front of the display plane (axis z = r_p,
    rim radius r_p + 1.2): clear them from the plate's side strips, x from the disc's inner face out."""
    r = p.r_outer + p.wall + p.ring_clear / 2 + 0.2
    out = None
    for sgn in (-1, 1):
        x0, x1 = sorted((sgn * (p.x_in - 0.1), sgn * (p.x_wall + 0.1)))
        c = Pos((x0 + x1) / 2, 0, p.front_wall + p.r_p) * Rot(0, 90, 0) * Cylinder(r, x1 - x0)
        out = c if out is None else out + c
    return out


def stop_lip(p: DrumParams):
    """Lip bar printed flat on its face: its lower edge S is the sharp edge on the bed.

    Local frame: x along the drum axis, y = z of the assembly, z = height above S (print up)."""
    w = 2 * p.x_wall - 0.2
    _, z_s = stop_edge(p)
    ledge = box(-w / 2, w / 2, z_s - p.lip_t, -p.front_wall, 0, p.lip_t)
    stem = box(-p.lip_w / 2, p.lip_w / 2, -p.front_wall - 0.01, z_s, 0, p.lip_t)
    return ledge + stem


def back_plate(p: DrumParams):
    """Back plate: plug slot, two registration pegs (pilot holes) at +-peg_x, inside face up."""
    w, h = 2 * p.x_wall, p.row_inner
    plate = box(-w / 2, w / 2, -h / 2, h / 2, 0, p.back_wall)
    plate = plate - box(-p.plug_slot_w / 2, p.plug_slot_w / 2, -p.plug_slot_h / 2, p.plug_slot_h / 2, -1, p.back_wall + 1)
    for sx in (-1, 1):
        peg = cyl(p.peg_pilot_d / 2 + 0.9, p.peg_h, p.back_wall, 0, sx * p.peg_x)
        plate = plate + peg - cyl(p.peg_pilot_d / 2, p.peg_h + 1, p.back_wall, 0, sx * p.peg_x)
    return plate
