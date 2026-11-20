"""Pure drum arithmetic (#673 section 1, 4, 6): no CAD, no params import (duck-typed)."""
import math


def chord(n: int, r: float) -> float:
    return 2 * r * math.sin(math.pi / n)


def min_pivot_radius(n: int, hole_d: float, web: float) -> float:
    """1a: smallest R_p with `web` of material between adjacent holes."""
    return (hole_d + web) / (2 * math.sin(math.pi / n))


def max_pivot_radius(row_inner: float, pivot_to_edge: float) -> float:
    """1c: guided pack, 2*sqrt(R_p^2 + L^2) <= row_inner."""
    half = row_inner / 2
    return math.sqrt(half * half - pivot_to_edge ** 2)


def hole_web(n: int, r: float, hole_d: float) -> float:
    return chord(n, r) - hole_d


def release_overlap(n: int, r: float) -> float:
    """4: the held flap's overlap o = chord/2, inside (0, chord)."""
    return chord(n, r) / 2


def stop_height(r_p: float, n: int, pivot_to_edge: float) -> float:
    """Height of the stop edge S above the midline, for the flap one pitch up."""
    phi = 2 * math.pi / n
    return r_p * math.sin(phi) + pivot_to_edge - release_overlap(n, r_p)


def stop_z(r_p: float, n: int, lean: float) -> float:
    phi = 2 * math.pi / n
    return r_p * (1 - math.cos(phi)) - lean


def pack_clearance(n: int, r_p: float, row_inner: float, pivot_to_edge: float,
                   thickness: float) -> float:
    """1e: air between tilted parallel flaps of the guided lower pack, minus t."""
    c = (row_inner / 2 - r_p) / pivot_to_edge
    if not -1 < c < 1:
        return float("-inf")
    return chord(n, r_p) * c - thickness


def rear_reach(p) -> float:
    return math.hypot(p.r_p, p.pivot_to_edge)


def depth_stack(p) -> list[tuple[str, float]]:
    """Layers front to back; the last three are the socket strip and backplane."""
    return [
        ("front wall", p.front_wall),
        ("mechanism to flap tips", p.r_p + rear_reach(p)),
        ("tip clearance", p.tip_clear),
        ("PSU module", p.psu_depth),
        ("insulating sheet", p.sheet_t),
        ("component gate", p.gate_h),
        ("board", p.board_pcb_t),
        ("rear standoff", p.standoff),
        ("back wall", p.back_wall),
        ("socket past the wall", p.socket_h - p.socket_sunk),
        ("backplane", p.backplane_t),
    ]


def depth_total(p) -> float:
    return sum(t for _, t in depth_stack(p))


def hub_free_length(p) -> float:
    """Hub interior beyond the motor and bulkhead (section 2): spans the discs' inner faces."""
    return 2 * p.x_in - p.motor_len - p.bulkhead_t


def boss_projection(p) -> float:
    """How far the far boss reaches past the far disc's inner face into the hub."""
    return p.boss_len - p.disc_t


def check(p) -> list[str]:
    """Every infeasible combination, as readable problems."""
    bad = []
    n = p.count
    if n < 8:
        bad.append("count must be >= 8")
        return bad
    if p.r_p < min_pivot_radius(n, p.hole_d, p.min_web):
        bad.append(f"r_p {p.r_p} < {min_pivot_radius(n, p.hole_d, p.min_web):.2f}: holes need min_web {p.min_web} between them")
    inner = p.row_inner
    if inner / 2 <= p.pivot_to_edge:
        bad.append("row too small for the flap")
    elif p.r_p > max_pivot_radius(inner, p.pivot_to_edge):
        bad.append(f"r_p {p.r_p} > {max_pivot_radius(inner, p.pivot_to_edge):.2f}: guided pack exceeds the row")
    if p.r_outer > inner / 2:
        bad.append("disc outer radius exceeds the row")
    if p.r_hub + p.min_web + p.hole_d / 2 > p.r_p:
        bad.append("motor bore leaves no ring between hub and pivot holes")
    if pack_clearance(n, p.r_p, inner, p.pivot_to_edge, p.flap.thickness) <= 0:
        bad.append("flaps cannot open in the lower pack (flap too thick for the pitch)")
    if p.frame_w > p.col_pitch:
        bad.append(f"frame width {p.frame_w:.2f} exceeds the column pitch {p.col_pitch}")
    if p.window_w > 2 * p.x_wall - 0.0 or p.window_h > inner:
        bad.append("window larger than the frame interior")
    if depth_total(p) > p.depth_budget:
        bad.append(f"depth stack {depth_total(p):.2f} exceeds {p.depth_budget}")
    if hub_free_length(p) - boss_projection(p) < 0:
        bad.append("encoder boss collides with the motor zone")
    if p.boss_od / 2 + p.min_web > p.r_bore - p.lug_w:
        bad.append("encoder boss reaches the screw lugs")
    return bad
