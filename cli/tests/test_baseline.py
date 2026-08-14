import json
import pathlib

import pytest
from splitflap_client.models import UnitsHealth
from splitflap_tui.baseline import (Baseline, delta_sxl, load_baseline,
                                    save_baseline, snapshot)

FIXDIR = pathlib.Path(__file__).parent / "fixtures"
UNITS = UnitsHealth.from_json(
    json.loads((FIXDIR / "units_health_esp32s3.json").read_text()))


def test_snapshot_keys_by_address_not_column():
    """Units are addressed, not positional — a re-addressed or re-ordered
    wall must still match its own baseline."""
    snap = snapshot(UNITS, rev="817e3a9")
    assert snap.rev == "817e3a9"
    assert snap.sxl[1] == 17          # address 1
    assert len(snap.sxl) == len(UNITS.units)


def test_delta_is_current_minus_baseline():
    snap = snapshot(UNITS, rev="r")
    moved = UnitsHealth.from_json({"units": [
        {"i": 0, "a": 1, "st": 1, "v": 1, "sxl": 20},      # +3
        {"i": 1, "a": 2, "st": 1, "v": 1, "sxl": 18},      # +0
    ]})
    assert delta_sxl(moved.units[0], snap) == 3
    assert delta_sxl(moved.units[1], snap) == 0


def test_delta_is_none_for_a_unit_the_baseline_never_saw():
    """A unit added (or re-addressed) since the baseline has nothing to be
    measured against — that must read as absent, not as a delta of 0."""
    snap = snapshot(UNITS, rev="r")
    added = UnitsHealth.from_json(
        {"units": [{"i": 0, "a": 99, "st": 1, "v": 1, "sxl": 5}]})
    assert delta_sxl(added.units[0], snap) is None


def test_delta_is_none_when_the_reading_itself_is_absent():
    snap = snapshot(UNITS, rev="r")
    silent = UnitsHealth.from_json({"units": [{"i": 0, "a": 1, "st": 0, "v": 0}]})
    assert delta_sxl(silent.units[0], snap) is None


def test_a_unit_whose_counter_went_backwards_reads_as_none():
    """sxl is a lifetime high-water mark, so it cannot decrease. If it does,
    the unit's EEPROM was erased or the address now points at different
    hardware — the baseline is meaningless for it, so report nothing rather
    than a negative number that looks like an improvement."""
    snap = snapshot(UNITS, rev="r")
    reset = UnitsHealth.from_json(
        {"units": [{"i": 0, "a": 1, "st": 1, "v": 1, "sxl": 2}]})
    assert delta_sxl(reset.units[0], snap) is None


def test_round_trip_through_disk(tmp_path):
    path = tmp_path / "wear-baseline.json"
    snap = snapshot(UNITS, rev="817e3a9")
    assert save_baseline(snap, path) is True
    loaded = load_baseline(path)
    assert loaded is not None
    assert loaded.rev == "817e3a9" and loaded.sxl == snap.sxl
    assert loaded.saved == snap.saved


def test_missing_file_loads_as_none():
    assert load_baseline(pathlib.Path("/nonexistent/wear-baseline.json")) is None


def test_corrupt_file_loads_as_none_rather_than_raising(tmp_path):
    path = tmp_path / "wear-baseline.json"
    path.write_text("{not json")
    assert load_baseline(path) is None


def test_unwritable_location_is_survivable(tmp_path):
    """Same rule as command history (#451): a read-only config dir must
    never take the app down."""
    path = tmp_path / "nope" / "deep" / "wear-baseline.json"
    path.parent.parent.mkdir()
    path.parent.parent.chmod(0o500)
    try:
        assert save_baseline(snapshot(UNITS, rev="r"), path) is False
    finally:
        path.parent.parent.chmod(0o700)


def test_baseline_from_a_payload_with_no_units_is_empty_not_broken():
    snap = snapshot(UnitsHealth.from_json({}), rev="")
    assert snap.sxl == {}
    assert isinstance(snap, Baseline)


@pytest.mark.parametrize("bad", ['{"units": 3}', '[]', '{"sxl": "no"}'])
def test_wrongly_shaped_files_load_as_none(tmp_path, bad):
    path = tmp_path / "wear-baseline.json"
    path.write_text(bad)
    assert load_baseline(path) is None


def test_default_path_is_resolved_per_call_not_bound_at_import(tmp_path,
                                                               monkeypatch):
    """A default argument is captured at import, so patching the module
    constant would be silently ignored and the suite would read and write
    the OPERATOR'S REAL baseline. conftest patches that constant, so this
    pins the only implementation shape that honours it."""
    import splitflap_tui.baseline as mod
    redirected = tmp_path / "redirected.json"
    monkeypatch.setattr(mod, "DEFAULT_BASELINE_PATH", redirected)
    assert mod.save_baseline(snapshot(UNITS, rev="r")) is True
    assert redirected.exists(), "save ignored the patched module constant"
    assert mod.load_baseline() is not None
    assert mod.clear_baseline() is True
    assert not redirected.exists()
