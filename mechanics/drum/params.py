"""Every drum and frame dimension, in mm (#673). Flap numbers come from flaps.params.

Assembly frame: x along the drum axis (motor side at -x), y up, z from the display plane
toward the back; the drum axis sits at (y, z) = (0, r_p). Each part is built in its own
print frame (see drum.py, frame.py) and placed by assembly.py.
"""
import math
from dataclasses import dataclass, field, fields, replace

from flaps.params import FlapParams

from drum import feasibility as fe


@dataclass(frozen=True)
class DrumParams:
    flap: FlapParams = field(default_factory=FlapParams)
    count: int = 64
    # pivot circle and discs
    r_p: float = 18.5
    hole_slop: float = 0.2
    min_web: float = 0.6
    rim_web: float = 0.6
    disc_t: float = 2.0
    g: float = 0.25
    # hub and motor
    wall: float = 1.2
    motor_d: float = 24.0
    motor_len: float = 19.0
    shaft_offset: float = 0.0
    bore_clear: float = 0.2
    shaft_d: float = 5.0
    shaft_flat: float = 3.0
    bulkhead_t: float = 1.2
    lug_t: float = 4.0
    lug_w: float = 5.0
    lug_count: int = 3
    screw_clear_d: float = 2.2
    screw_pilot_d: float = 1.7
    spigot_len: float = 1.5
    spigot_clear: float = 0.1
    # far-end axle, magnet, post
    post_d: float = 9.0
    post_len: float = 6.4
    boss_bore_clear: float = 0.4
    boss_od: float = 12.0
    board_t: float = 0.8
    chip_t: float = 1.75
    encoder_gap: float = 1.0
    magnet_d: float = 6.0
    magnet_t: float = 2.5
    magnet_slop: float = 0.1
    cap_t: float = 1.2
    ffc_slot_w: float = 4.5
    ffc_slot_t: float = 1.0
    groove_w: float = 5.5
    groove_depth: float = 0.6
    # frame
    side_gap: float = 0.4
    ring_clear: float = 0.4
    ring_len: float = 2.0
    col_pitch: float = 41.5
    row_pitch: float = 73.0
    window_w: float = 34.0
    window_h: float = 58.0
    stop_dz: float = 1.0
    lip_t: float = 1.2
    lip_w: float = 36.0
    plug_slot_w: float = 10.0
    plug_slot_h: float = 4.0
    peg_x: float = 7.0
    peg_pilot_d: float = 1.7
    peg_h: float = 3.0
    # depth stack (z from the display plane)
    front_wall: float = 1.2
    tip_clear: float = 0.6
    psu_depth: float = 26.0
    sheet_t: float = 0.5
    gate_h: float = 8.0
    board_pcb_t: float = 1.6
    standoff: float = 2.7
    back_wall: float = 1.2
    socket_h: float = 4.0
    socket_sunk: float = 1.2
    backplane_t: float = 1.6
    depth_budget: float = 101.0
    tess_tol: float = 0.02

    def __post_init__(self):
        bad = []
        if not all(math.isfinite(getattr(self, f.name)) for f in fields(self) if f.name != "flap"):
            bad.append("every parameter must be a finite number")
        else:
            bad += fe.check(self)
        if bad:
            raise ValueError("; ".join(bad))

    def with_overrides(self, **kw: float) -> "DrumParams":
        known = {f.name for f in fields(self)} - {"flap"}
        unknown = sorted(set(kw) - known)
        if unknown:
            raise ValueError(f"unknown parameter(s): {', '.join(unknown)}")
        return replace(self, **kw)

    # derived
    @property
    def hole_d(self) -> float:
        return self.flap.pin_d + self.hole_slop

    @property
    def pivot_to_edge(self) -> float:
        return self.flap.height - self.flap.pivot_y

    @property
    def r_outer(self) -> float:
        return self.r_p + self.hole_d / 2 + self.rim_web

    @property
    def r_bore(self) -> float:
        return self.motor_d / 2 + self.shaft_offset + self.bore_clear

    @property
    def r_hub(self) -> float:
        return self.r_bore + self.wall

    @property
    def x_in(self) -> float:
        """Disc inner face, distance from the drum centre plane."""
        return self.flap.narrow_width / 2 + self.g

    @property
    def x_out(self) -> float:
        return self.x_in + self.disc_t

    @property
    def x_joint(self) -> float:
        """Joint plane: past the motor, the bulkhead and half A's lug layer."""
        return -self.x_in + self.motor_len + self.bulkhead_t + self.lug_t

    @property
    def x_wall(self) -> float:
        """Inner face of a side wall."""
        return self.x_out + self.side_gap

    @property
    def frame_w(self) -> float:
        return 2 * (self.x_wall + self.wall)

    @property
    def boss_bore_d(self) -> float:
        return self.post_d + self.boss_bore_clear

    @property
    def boss_len(self) -> float:
        """Boss length from the far disc's outer face: bore, magnet, cap."""
        return self.magnet_x_from_wall + self.magnet_t + self.cap_t - self.side_gap

    @property
    def magnet_x_from_wall(self) -> float:
        """Magnet near face, measured from the far wall's inner face (section 7)."""
        return self.post_len + self.board_t + self.chip_t + self.encoder_gap

    @property
    def row_inner(self) -> float:
        return self.row_pitch - 2 * self.wall

    @property
    def z_back(self) -> float:
        """Inner face of the back wall."""
        return sum(t for _, t in fe.depth_stack(self)[:-3]) - self.front_wall
