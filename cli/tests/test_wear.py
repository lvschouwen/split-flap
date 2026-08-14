"""#455: pure lifetime-wear markers for the units table."""
from splitflap_tui.wear import DRAG_FLOOR, row_sxl_median, unit_markers


class U:
    """Minimal stand-in for splitflap_client's Unit (only what wear reads)."""

    def __init__(self, sxl=None, hf=None, stale=False, fault=False):
        self.sxl = sxl
        self.hall_fails = hf
        self.stale = stale
        self.fault = fault


# ---- median ------------------------------------------------------------
def test_median_of_odd_count():
    assert row_sxl_median([U(17), U(972), U(18)]) == 18


def test_median_of_even_count():
    assert row_sxl_median([U(10), U(20), U(30), U(40)]) == 25


def test_median_ignores_units_that_omit_sxl():
    # Key emission is validity-gated firmware-side, so absent is normal.
    assert row_sxl_median([U(17), U(None), U(19)]) == 18


def test_median_is_none_when_no_unit_reports_sxl():
    assert row_sxl_median([U(None), U(None)]) is None


def test_median_of_empty_row_is_none():
    assert row_sxl_median([]) is None


# ---- HALL --------------------------------------------------------------
def test_hall_marker_on_any_hall_failure():
    assert "HALL" in unit_markers(U(hf=1), median=17)


def test_no_hall_marker_at_zero_or_absent():
    assert "HALL" not in unit_markers(U(hf=0), median=17)
    assert "HALL" not in unit_markers(U(hf=None), median=17)


# ---- DRAG --------------------------------------------------------------
def test_drag_marker_on_the_real_a15_signature():
    # a15: sxl 1465 against a row median of 17 — 86x.
    assert "DRAG" in unit_markers(U(sxl=1465), median=17)


def test_healthy_unit_at_the_fleet_norm_is_clean():
    assert unit_markers(U(sxl=17), median=17) == []
    assert unit_markers(U(sxl=37), median=17) == []


def test_drag_needs_both_the_multiple_and_the_floor():
    # 8x a tiny median is still a tiny number — the floor stops that.
    assert "DRAG" not in unit_markers(U(sxl=DRAG_FLOOR - 1), median=2)
    assert "DRAG" in unit_markers(U(sxl=DRAG_FLOOR), median=2)


def test_high_absolute_but_within_the_multiple_is_clean():
    # A uniformly worn row must not flag every unit.
    assert "DRAG" not in unit_markers(U(sxl=900), median=800)


def test_drag_falls_back_to_the_floor_when_the_median_is_useless():
    for bad in (None, 0):
        assert "DRAG" in unit_markers(U(sxl=DRAG_FLOOR), median=bad)
        assert "DRAG" not in unit_markers(U(sxl=DRAG_FLOOR - 1), median=bad)


def test_absent_sxl_never_drags():
    assert "DRAG" not in unit_markers(U(sxl=None), median=17)


# ---- composition -------------------------------------------------------
def test_a6_carries_both_markers():
    # The real a6: hf=3 AND sxl 972 against a median of 17. The flat table
    # showed neither; it has both signatures, not just the hall one.
    assert unit_markers(U(sxl=972, hf=3), median=17) == ["HALL", "DRAG"]


def test_liveness_flags_lead_the_wear_markers():
    assert unit_markers(U(sxl=1465, stale=True), median=17)[0] == "STALE"
    assert unit_markers(U(sxl=1465, fault=True), median=17)[0] == "FAULT"


def test_stale_wins_over_fault():
    marks = unit_markers(U(stale=True, fault=True), median=17)
    assert marks == ["STALE"]


def test_single_unit_row_never_drags_against_itself():
    only = U(sxl=1465)
    assert "DRAG" not in unit_markers(only, median=row_sxl_median([only]))


def test_the_median_rule_needs_a_majority_healthy_row():
    """Documented consequence, not an oversight: when half a row sits at the
    high value there is no longer a healthy baseline to be an outlier from,
    and the rule declines to call half a row anomalous. It is the same
    property that stops a uniformly aged wall lighting up end to end."""
    half_bad = [U(17), U(17), U(972), U(1465)]
    median = row_sxl_median(half_bad)          # (17 + 972) // 2 = 494
    assert median == 494
    assert unit_markers(U(sxl=972), median=median) == []

    # Add the other twelve healthy units back and the outliers reappear.
    full = half_bad + [U(18)] * 12
    assert row_sxl_median(full) == 18
    assert "DRAG" in unit_markers(U(sxl=972), median=row_sxl_median(full))
