"""Plate packing: a grid of flap footprints on the bed, with an optional prime-tower keep-out.

Pure (no CAD import). A footprint is the flap's width x height; Placed.x/.y is
its min corner on the bed.
"""
from dataclasses import dataclass
from typing import Sequence

from flaps.params import FlapParams


@dataclass(frozen=True)
class Placed:
    flap: int
    x: float
    y: float


def _cols(p: FlapParams) -> int:
    return int((p.bed - 2 * p.bed_margin + p.spacing) // (p.width + p.spacing))


def _slots(p: FlapParams, tower: bool) -> list[tuple[float, float]]:
    cols = _cols(p)
    rows = int((p.bed - 2 * p.bed_margin + p.spacing) // (p.height + p.spacing))
    keep_out = p.bed - p.bed_margin - p.prime_tower - p.spacing
    slots = []
    for r in range(rows):
        for c in range(cols):
            x = p.bed_margin + c * (p.width + p.spacing)
            y = p.bed_margin + r * (p.height + p.spacing)
            if tower and x + p.width > keep_out and y + p.height > keep_out:
                continue
            slots.append((x, y))
    return slots


def pack(flaps: Sequence[int], p: FlapParams, tower: bool) -> list[list[Placed]]:
    slots = _slots(p, tower)
    if not slots:
        raise ValueError("no flap fits on the bed with these parameters")
    plates = []
    for start in range(0, len(flaps), len(slots)):
        chunk = flaps[start:start + len(slots)]
        plates.append([Placed(f, x, y) for f, (x, y) in zip(chunk, slots)])
    return plates


def grid(flaps: Sequence[int], p: FlapParams) -> list[Placed]:
    """One bed-wide grid that runs past the bed in y, for the slicer's auto-arrange to spread."""
    cols = _cols(p)
    if cols < 1:
        raise ValueError("no flap fits on the bed with these parameters")
    return [Placed(f, p.bed_margin + (k % cols) * (p.width + p.spacing),
                   p.bed_margin + (k // cols) * (p.height + p.spacing))
            for k, f in enumerate(flaps)]
