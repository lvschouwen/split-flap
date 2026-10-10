"""Source gates for two recoveries a native test cannot reach.

A stalled firmware upload thaws the row (#503): everything the upload took —
the TX power cap, the session slot, the updater and its buffer — goes back.

A re-probe of a dead bus closes nothing on an address ACK alone (#496): only a
unit that sent a reply that checked out does (a bootloader's, or a version),
everything else waits for a status read (FollowerBusRecovery.h, natively tested).
"""
import re
from pathlib import Path

TREE = Path(__file__).resolve().parents[1]


def _code(name):
    return re.sub(r"//[^\n]*", "", (TREE / name).read_text())


def _block(code, start, end="\n}\n"):
    body = code[code.index(start):]
    return body[:body.index(end)]


def test_a_stalled_upload_gives_everything_back():
    thaw = _block(_code("FollowerWeb.cpp"), "bool webOtaUploadFrozen()")
    stalled = thaw[thaw.index("otaUploadStalled("):]
    for step in ("masterOtaUploadActive = false;",
                 "followerTxOtaCap(false);",
                 "masterOtaOwnerRequest = nullptr;",
                 "Update.end(false);"):
        assert step in stalled, step
    # The slot is freed first: a late chunk must find no owner, not an
    # updater that was just ended.
    assert stalled.index("masterOtaOwnerRequest = nullptr;") < stalled.index("Update.end(false);")
    # One end() on a latched error leaves the session open.
    assert stalled.count("Update.end(false);") == 2


def test_a_reprobe_does_not_close_the_episode_on_an_address_ack():
    tick = _block(_code("FollowerBus.cpp"), "static void followerBusRecoveryTick()")
    # A row is empty while no unit has ANSWERED (#496): an address that only
    # acknowledged at a scan must not stop the re-probing.
    assert "followerUnitsAnswering(unitFacts, UNITS_AMOUNT)" in tick
    assert "busRecoveryReprobeDue(busRecovery, detectedUnitCount)" not in tick
    reprobe = tick[tick.index("busRecoveryReprobeDue(busRecovery, answering)"):]
    assert "busRecoveryNoteReprobe(busRecovery, detectedUnitCount);" in reprobe
    closes = re.findall(r"if \(([^)]*)\) \{\s*observeLiveness\(0, true\);", reprobe)
    assert closes == ["unitReplied"], closes
    assert reprobe.count("observeLiveness(0, true)") == 1


def test_an_empty_row_is_judged_by_units_that_answered_not_by_acknowledges():
    beat = _block(_code("FollowerBus.cpp"), "void followerHeartbeatTick()")
    assert "if (followerUnitsAnswering(unitFacts, UNITS_AMOUNT) == 0) {" in beat
    assert "if (detectedUnitCount == 0) busRecoveryNoteEmptyRow" not in beat


def test_a_bus_death_is_recorded_once_at_its_first_attempt():
    tick = _block(_code("FollowerBus.cpp"), "static void followerBusRecoveryTick()")
    # Counted before the attempt is noted, recorded once per episode.
    assert tick.index("followerUnitsAnswering(") < tick.index("busRecoveryNoteAttempt(")
    assert tick.count("recordBusDeath(") == 1
    assert "if (busRecovery.attemptsThisEpisode == 1) recordBusDeath(status, answering);" in tick
    record = _block(_code("FollowerBus.cpp"), "static void recordBusDeath(")
    for code in ("ROW_EVT_BUS_DEAD", "ROW_EVT_BUS_LINES"):
        assert f"wl_RowEventCode_{code}" in record, code


def test_the_line_test_runs_only_on_an_idle_bus_and_puts_the_pull_up_back():
    code = _code("FollowerLineTest.cpp")
    rise = _block(code, "uint16_t riseOf(uint8_t pin)")
    assert rise.index("GPF(pin) &= ~(1 << GPFPU);") < rise.index("GPES = mask;")
    assert rise.index("GPEC = mask;") < rise.index("GPF(pin) |= (1 << GPFPU);")
    # Interrupts are off only around the timed part, and always come back.
    assert rise.count("noInterrupts();") == 1 and rise.count("  interrupts();") == 1
    assert rise.index("noInterrupts();") < rise.index("GPEC = mask;") < rise.index("  interrupts();")
    test = _block(code, "FollowerLineReading followerLineTest()")
    assert "if ((GPI & both) != both) return r;" in test
    healthy = _block(_code("FollowerBus.cpp"), "static void busLinesHealthyTick(")
    assert "if (busRowMoving()) return;" in healthy
