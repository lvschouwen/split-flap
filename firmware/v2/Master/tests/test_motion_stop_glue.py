"""Source gate for a stop at the motion gate (#510).

The radio-quiet wait ends early when a stop is pending. Whatever asked for the
wait must then start nothing: a home, jog or self-test queued ahead of the
stop would otherwise move a unit the operator has just stopped.
"""
import re
from pathlib import Path

MASTER = Path(__file__).resolve().parent.parent


def _code(path):
    return re.sub(r"//[^\n]*", "", path.read_text())


def test_the_gate_reports_a_pending_stop():
    code = _code(MASTER / "UnitBus.cpp")
    body = code[code.index("static bool admitMotion()"):]
    body = body[:body.index("\n}")]
    assert "motionGate()" in body
    assert "return !abortRequested.load();" in body


def test_no_mover_ignores_the_gate():
    code = _code(MASTER / "UnitBus.cpp")
    calls = re.findall(r"^[^\n]*admitMotion\(\)[^\n]*$", code, re.M)
    uses = [c for c in calls if "static bool admitMotion()" not in c]
    assert len(uses) == 4, uses  # frame, jog, home, self-test
    for line in uses:
        assert re.search(r"if \(!admitMotion\(\)\) return", line), line


def test_a_stopped_mover_is_not_graded_as_a_bus_failure():
    code = _code(MASTER / "DisplayTask.cpp")
    assert code.count("gradeMover(status)") == 2  # jog, home
    assert "started == UNIT_BUS_STOPPED" in code
