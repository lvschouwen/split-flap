from pathlib import Path
from flasher.session import (Session, clear_session, load_session, next_unit,
                             save_session)


def test_next_unit_walks_in_order():
    s = Session(unit_count=3)
    assert next_unit(s) == 1
    s.done.append(1)
    assert next_unit(s) == 2


def test_skipped_units_revisited_after_untouched_ones():
    s = Session(unit_count=3, done=[1], skipped=[2])
    assert next_unit(s) == 3          # untouched first
    s.done.append(3)
    assert next_unit(s) == 2          # then the skipped one
    s.done.append(2)
    assert next_unit(s) is None       # complete (even though still in skipped list)


def test_roundtrip(tmp_path):
    p = tmp_path / "s.json"
    save_session(Session(unit_count=12, done=[1, 2], skipped=[3], programmer_port="COM4"), p)
    s = load_session(p)
    assert s.unit_count == 12 and s.done == [1, 2] and s.skipped == [3]
    assert s.programmer_port == "COM4"


def test_load_missing_or_corrupt_returns_none(tmp_path):
    assert load_session(tmp_path / "nope.json") is None
    bad = tmp_path / "bad.json"
    bad.write_text("{not json")
    assert load_session(bad) is None


def test_clear_session_removes_file(tmp_path):
    p = tmp_path / "s.json"
    save_session(Session(unit_count=3, done=[1, 2, 3]), p)
    assert p.exists()
    clear_session(p)
    assert not p.exists()


def test_clear_session_missing_file_is_noop(tmp_path):
    clear_session(tmp_path / "nope.json")  # must not raise
