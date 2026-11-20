"""Small build123d helpers shared by drum.py and frame.py. Print frame: z up from the bed."""
import math

from build123d import Align, Box, Cylinder, Pos, Rot

MIN = (Align.CENTER, Align.CENTER, Align.MIN)


def cyl(r: float, h: float, z0: float = 0.0, x: float = 0.0, y: float = 0.0):
    return Pos(x, y, z0) * Cylinder(r, h, align=MIN)


def tube(r_out: float, r_in: float, h: float, z0: float = 0.0):
    return cyl(r_out, h, z0) - cyl(r_in, h + 2, z0 - 1)


def box(x0: float, x1: float, y0: float, y1: float, z0: float, z1: float):
    return Pos((x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2) * Box(x1 - x0, y1 - y0, z1 - z0)


def polar(r: float, deg: float) -> tuple[float, float]:
    a = math.radians(deg)
    return r * math.cos(a), r * math.sin(a)


def rot_z(shape, deg: float):
    return Rot(0, 0, deg) * shape
