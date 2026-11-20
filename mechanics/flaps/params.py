"""Every flap dimension, in mm. Nothing else in the generator holds a literal size.

Flap coordinates: y = 0 is the hinge edge (the edge with the pivot tabs), the
flap reaches to y = height; x is centred. The pivot axis (y = pivot_y, mid-thickness)
runs through two round pins and sits on the display's horizontal midline.
"""
import math
from dataclasses import dataclass, fields, replace

MIN_TAB_REACH = 1.5
MIN_PIN_D = 1.0
MAX_CAP_FRAC = 0.9


@dataclass(frozen=True)
class FlapParams:
    width: float = 36.0
    height: float = 31.0
    thickness: float = 0.8
    narrow_width: float = 32.5
    narrow_height: float = 16.0
    pin_d: float = 1.0
    pivot_y: float = 1.8
    inlay_depth: float = 0.2
    cap_frac: float = 0.70
    edge_margin: float = 1.0
    stroke_offset: float = 0.0
    bed: float = 256.0
    bed_margin: float = 5.0
    spacing: float = 3.0
    prime_tower: float = 35.0
    tess_tol: float = 0.02

    def __post_init__(self):
        problems = []
        if not all(math.isfinite(getattr(self, f.name)) for f in fields(self)):
            problems.append("every parameter must be a finite number")
        for name in ("thickness", "spacing", "prime_tower", "tess_tol"):
            if not getattr(self, name) > 0:
                problems.append(f"{name} must be > 0")
        if self.bed_margin < 0:
            problems.append("bed_margin must be >= 0")
        if not 0 <= self.edge_margin < self.narrow_width / 4:
            problems.append("edge_margin must be >= 0 and leave room for a glyph")
        if not 0 < self.inlay_depth < self.thickness / 2:
            problems.append("inlay_depth must be > 0 and < thickness/2 (the faces' pockets would meet)")
        if self.tab_reach < MIN_TAB_REACH:
            problems.append(f"tab reach (width - narrow_width)/2 must be >= {MIN_TAB_REACH}")
        if self.pin_d < MIN_PIN_D:
            problems.append(f"pin_d must be >= {MIN_PIN_D}")
        if self.pin_d < self.thickness:
            problems.append("pin_d must be >= thickness (the pin is truncated flat to the thickness)")
        if not 0 < self.cap_frac <= MAX_CAP_FRAC:
            problems.append(f"cap_frac must be in (0, {MAX_CAP_FRAC}]")
        if not self.pin_d / 2 < self.pivot_y < self.narrow_height - self.pin_d / 2:
            problems.append("pins must sit inside the narrow zone (0 < pivot_y - pin_d/2, pivot_y + pin_d/2 < narrow_height)")
        if not self.narrow_height < self.height:
            problems.append("the narrow zone must be shorter than the flap")
        if problems:
            raise ValueError("; ".join(problems))

    @property
    def tab_reach(self) -> float:
        return (self.width - self.narrow_width) / 2

    @property
    def char_height(self) -> float:
        return 2 * self.height

    def with_overrides(self, **kw: float) -> "FlapParams":
        known = {f.name for f in fields(self)}
        unknown = sorted(set(kw) - known)
        if unknown:
            raise ValueError(f"unknown parameter(s): {', '.join(unknown)}")
        return replace(self, **kw)
