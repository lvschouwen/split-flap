"""Pure lifetime-wear markers for the units table (#455). No I/O.

The board's own `faulty` count answers "is this unit responding?", not "is
this unit wearing out" — a wall can report `faulty: 0` while carrying units
that need physical attention. These markers read the lifetime counters the
firmware already exposes (`sxl`, `hf`) and call out the outliers.

DRAG is relative to the row median rather than an absolute number, mirroring
the firmware's own relative-wear approach for odometers: a wall that ages
evenly should not light up every row, and a row that has been re-calibrated
shifts its own baseline without anyone editing a constant here.
"""
from __future__ import annotations

DRAG_MEDIAN_MULTIPLE = 8
"""How far above its row's median steps-to-home a unit must sit to be called
out. a15 sits at 86x; the healthy units sit at 1x."""

DRAG_FLOOR = 100
"""Absolute floor, so a row whose median is near zero cannot make trivially
small excursions look catastrophic. Also the fallback when the median is
unusable."""

HALL_FAIL_MIN = 1
"""Any recorded hall failure is worth surfacing — the escalation bar sits at
3, but an operator wants to see the count climbing before it gets there."""


def row_sxl_median(units) -> int | None:
    """Median lifetime steps-to-home across a row, or None if no unit
    reports one. Units that omit `sxl` are skipped rather than counted as
    zero — key emission is validity-gated firmware-side, so absent means
    "not measured", not "measured as nothing"."""
    values = sorted(u.sxl for u in units if u.sxl is not None)
    if not values:
        return None
    mid = len(values) // 2
    if len(values) % 2:
        return values[mid]
    return (values[mid - 1] + values[mid]) // 2


def _is_drag(sxl: int | None, median: int | None) -> bool:
    if sxl is None or sxl < DRAG_FLOOR:
        return False
    if not median or median <= 0:
        return True          # no usable baseline — the floor is the whole test
    return sxl >= DRAG_MEDIAN_MULTIPLE * median


def unit_markers(unit, median: int | None) -> list[str]:
    """Flag-column contents for one unit, most urgent first.

    Liveness leads: a unit that is stale or faulted needs looking at before
    anything its wear counters say (and those counters may be a stale read).
    A unit can legitimately carry more than one wear marker — the real a6
    has both an elevated `sxl` and a hall-failure count."""
    if unit.stale:
        return ["STALE"]
    marks = ["FAULT"] if unit.fault else []
    hf = unit.hall_fails
    if hf is not None and hf >= HALL_FAIL_MIN:
        marks.append("HALL")
    if _is_drag(unit.sxl, median):
        marks.append("DRAG")
    return marks
