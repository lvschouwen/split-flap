"""Plate packing: a grid of flap footprints on the bed, with an optional prime-tower keep-out.

Pure (no CAD import). A footprint is the flap's width x height; Placed.x/.y is
its min corner on the bed.
"""
from dataclasses import dataclass
from typing import Sequence

from flaps.alphabet import Entry, uses_only_white
from flaps.layout import faces_of_flap
from flaps.params import FlapParams


@dataclass(frozen=True)
class Placed:
    flap: int
    x: float
    y: float


def split_by_colour(alphabet: Sequence[Entry]) -> tuple[list[int], list[int]]:
    """(flaps whose faces use only white or nothing, the rest)."""
    plain, coloured = [], []
    for i in range(len(alphabet)):
        (plain if all(uses_only_white(e) for e in faces_of_flap(i, alphabet)) else coloured).append(i)
    return plain, coloured


def _slots(p: FlapParams, tower: bool) -> list[tuple[float, float]]:
    usable = p.bed - 2 * p.bed_margin
    cols = int((usable + p.spacing) // (p.width + p.spacing))
    rows = int((usable + p.spacing) // (p.height + p.spacing))
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
