"""Flap solids: a black body with flush inlay pockets and one solid per colour.

The front face sits at z = 0 (printed on the plate), the back face at
z = thickness. A glyph is drawn once in display coordinates (centred on the
midline, cap height = cap_frac * char_height), split at the midline, and each
half is mapped onto its flap face as `layout.display_to_flap` describes.
"""
import math
from dataclasses import dataclass
from functools import lru_cache
from pathlib import Path
from typing import Literal, Optional, Sequence

from build123d import (Align, Part, Plane, Polygon, Pos, Rectangle, Sketch, Text,
                       extrude, mirror, offset, scale)

from flaps.alphabet import Entry, inlay_colour
from flaps.layout import faces_of_flap
from flaps.params import FlapParams

FONT = Path(__file__).with_name("fonts") / "BarlowCondensed-SemiBold.ttf"
_REF_SIZE = 10.0
_BIG = 1000.0
_MIN_AREA = 1e-6
_MAX_CLIP = 0.005  # fraction of a glyph half that may be trimmed by the edge
_C, _MIN_ALIGN, _MAX_ALIGN = Align.CENTER, Align.MIN, Align.MAX

Half = Literal["top", "bottom"]


@dataclass(frozen=True)
class FlapParts:
    body: Part
    inlays: dict[str, Part]


def flap_outline(p: FlapParams) -> Sketch:
    nw, w = p.narrow_width / 2, p.width / 2
    t0, t1 = p.tab_y0, p.tab_y0 + p.tab_h
    right = [(nw, 0), (nw, t0), (w, t0), (w, t1), (nw, t1), (nw, p.narrow_height),
             (w, p.narrow_height), (w, p.height)]
    left = [(-x, y) for x, y in reversed(right)]
    return Sketch() + Polygon(*right, *left, align=None)


def inlay_region(p: FlapParams) -> Sketch:
    """Where an inlay may go: the outline inset by edge_margin, above the pivot axis."""
    inset = offset(flap_outline(p), -p.edge_margin)
    above_pivot = Pos(0, p.pivot_y) * Rectangle(_BIG, _BIG, align=(_C, _MIN_ALIGN))
    return inset & above_pivot


def max_glyph_width(p: FlapParams) -> float:
    return p.narrow_width - 2 * p.edge_margin


@lru_cache(maxsize=None)
def _cap_metrics() -> tuple[float, float]:
    """(cap height, cap centre y) of 'H' per unit font size."""
    bb = Text("H", _REF_SIZE, font_path=str(FONT)).bounding_box()
    return bb.size.Y / _REF_SIZE, (bb.min.Y + bb.max.Y) / 2 / _REF_SIZE


def _char_sketch(c: str, p: FlapParams) -> Sketch:
    cap_per_size, centre_per_size = _cap_metrics()
    size = p.cap_frac * p.char_height / cap_per_size
    text = Text(c, size, font_path=str(FONT))
    bb = text.bounding_box()
    glyph = Pos(-(bb.min.X + bb.max.X) / 2, -centre_per_size * size) * text
    if p.stroke_offset:
        try:
            glyph = offset(glyph, p.stroke_offset)
        except RuntimeError as e:
            raise ValueError(f"stroke_offset {p.stroke_offset} fails on {c!r}: {e}") from e
    return glyph


def _heart_sketch(p: FlapParams) -> Sketch:
    """One face from the parametric heart curve, scaled to the cap height."""
    n = 160
    pts = []
    for k in range(n):
        t = 2 * math.pi * k / n
        pts.append((16 * math.sin(t) ** 3,
                    13 * math.cos(t) - 5 * math.cos(2 * t) - 2 * math.cos(3 * t) - math.cos(4 * t)))
    xs, ys = [x for x, _ in pts], [y for _, y in pts]
    cap = p.cap_frac * p.char_height
    k_y = cap / (max(ys) - min(ys))
    k_x = min(k_y, max_glyph_width(p) / (max(xs) - min(xs)))
    y_mid = (max(ys) + min(ys)) / 2
    ccw = reversed(pts)  # the curve runs clockwise; a face must face +z
    return Sketch() + Polygon(*[(x * k_x, (y - y_mid) * k_y) for x, y in ccw], align=None)


def _squeeze_to_fit(glyph: Sketch, p: FlapParams) -> Sketch:
    width = glyph.bounding_box().size.X
    limit = max_glyph_width(p)
    if width <= limit:
        return glyph
    return scale(glyph, by=(limit / width, 1, 1))


@lru_cache(maxsize=None)
def glyph_display(entry: Entry, p: FlapParams) -> Optional[Sketch]:
    if entry.kind == "blank":
        return None
    if entry.kind == "colour":
        return Sketch() + Rectangle(p.width, p.char_height)
    glyph = _heart_sketch(p) if entry.kind == "heart" else _char_sketch(entry.value, p)
    return _squeeze_to_fit(glyph, p)


def half_on_flap(entry: Entry, half: Half, p: FlapParams) -> Optional[Sketch]:
    glyph = glyph_display(entry, p)
    if glyph is None:
        return None
    side = (_MIN_ALIGN if half == "top" else _MAX_ALIGN)
    part = glyph & Rectangle(_BIG, _BIG, align=(_C, side))
    if part.area <= _MIN_AREA:
        return None
    part = mirror(part, Plane.YZ)  # both faces are read from -z (layout.py)
    if half == "bottom":
        part = mirror(part, Plane.XZ)
    placed = Pos(0, p.pivot_y) * part
    mapped = placed & inlay_region(p)
    if entry.kind != "colour" and mapped.area < placed.area * (1 - _MAX_CLIP):
        raise ValueError(f"{entry.kind} {entry.value!r} {half} half clipped by the flap edge; "
                         "lower cap_frac or stroke_offset")
    return mapped if mapped.area > _MIN_AREA else None


def _pocket(sketch: Sketch, z0: float, p: FlapParams) -> Part:
    return Pos(0, 0, z0) * extrude(sketch, p.inlay_depth, dir=(0, 0, 1))


def build_flap(i: int, alphabet: Sequence[Entry], p: FlapParams) -> FlapParts:
    front_entry, back_entry = faces_of_flap(i, alphabet)
    faces = [(front_entry, half_on_flap(front_entry, "top", p), 0.0),
             (back_entry, half_on_flap(back_entry, "bottom", p), p.thickness - p.inlay_depth)]
    body = extrude(flap_outline(p), p.thickness)
    inlays: dict[str, Part] = {}
    for entry, sketch, z0 in faces:
        if sketch is None:
            continue
        solid = _pocket(sketch, z0, p)
        body = body - solid
        colour = inlay_colour(entry)
        inlays[colour] = inlays[colour] + solid if colour in inlays else solid
    return FlapParts(body=body, inlays=inlays)

