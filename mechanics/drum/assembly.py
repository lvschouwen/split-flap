"""All printable parts of the unit, their print frames and their assembly poses (#673 section 9)."""
from dataclasses import dataclass

from build123d import Location, Plane, Pos

from drum import drum as dr
from drum import frame as fr
from drum.params import DrumParams


@dataclass(frozen=True)
class Part:
    name: str
    solid: object      # print frame
    pose: Location     # print frame -> assembly frame
    essential: bool = True


def _pose(origin, x_dir, z_dir) -> Location:
    return Location(Plane(origin=origin, x_dir=x_dir, z_dir=z_dir))


def parts(p: DrumParams, sleeve_d: float = 15.2, sleeve_len: float = 16.2) -> list[Part]:
    halves = dr.build_halves(p)
    xo, xw, h = p.x_out, p.x_wall, p.row_pitch / 2
    z_end = p.z_back + p.back_wall
    back, fwd = (0, 0, 1), (1, 0, 0)
    return [
        Part("drum-a", halves.a, _pose((-xo, 0, p.r_p), back, fwd)),
        Part("drum-b", halves.b, _pose((xo, 0, p.r_p), back, (-1, 0, 0))),
        Part("sleeve", dr.sleeve(p, sleeve_d, sleeve_len), _pose((-p.x_in, 0, p.r_p), back, fwd), False),
        Part("wall-motor", fr.side_wall(p, False), _pose((-(xw + p.wall), 0, 0), back, fwd)),
        Part("wall-far", fr.side_wall(p, True), _pose((xw + p.wall, 0, 0), back, (-1, 0, 0))),
        Part("bearing-ring", fr.bearing_ring(p), _pose((-xw, 0, p.r_p), back, fwd)),
        Part("rail-bottom", fr.rail(p), _pose((0, -h, 0), back, (0, 1, 0))),
        Part("rail-top", fr.rail(p), _pose((0, h, 0), back, (0, -1, 0))),
        Part("front-plate", fr.front_plate(p), Pos(0, 0, -p.front_wall)),
        Part("back-plate", fr.back_plate(p), _pose((0, 0, z_end), (-1, 0, 0), (0, 0, -1))),
        Part("stop-lip", fr.stop_lip(p), _pose((0, fr.stop_edge(p)[0], 0), (-1, 0, 0), (0, 1, 0))),
    ]


def assembled(p: DrumParams) -> list[tuple[str, object]]:
    """Each part moved into the assembly frame."""
    return [(q.name, q.pose * q.solid) for q in parts(p)]
