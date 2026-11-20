"""Which character half lands on which flap face, and where.

Character i shows its top half on the front of flap i and its bottom half on
the back of flap i-1 (circular). Display coordinates: origin at the character
centre on the midline, +y up. The front face prints on the bed (z = 0) and is
read from -z, so screen right is flap -x. A fallen flap is the raised flap
turned 180° about the hinge (x) axis, which keeps x, so its back is read with
x mirrored too. Distance from the midline maps to distance from the pivot axis
on either face.
"""
from typing import Literal, Sequence

from flaps.alphabet import Entry
from flaps.params import FlapParams

Face = Literal["front", "back"]


def faces_of_flap(i: int, alphabet: Sequence[Entry]) -> tuple[Entry, Entry]:
    """(entry whose top half is on the front, entry whose bottom half is on the back)."""
    return alphabet[i], alphabet[(i + 1) % len(alphabet)]


def display_to_flap(x: float, y: float, p: FlapParams) -> tuple[Face, float, float]:
    if y >= 0:
        return "front", -x, p.pivot_y + y
    return "back", -x, p.pivot_y - y
