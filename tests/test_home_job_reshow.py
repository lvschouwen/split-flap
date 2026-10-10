"""A job that leaves a drum at home shows the row's text again (#575).

Both boards keep the frame they last showed. A single-unit home, a self-test
and anything that sent a unit through its bootloader leave that unit at home
or unhomed; each of those paths must hand the row its frame back, or the unit
stands at blank until the text next changes. Board glue, so pinned by source.
"""
import re
from pathlib import Path

V2 = Path(__file__).resolve().parents[1] / "firmware/v2"
MASTER = (V2 / "Master/DisplayTask.cpp").read_text()
ROW_JOBS = (V2 / "FollowerEsp01/FollowerUnitJobs.cpp").read_text()
ROW_BUS = (V2 / "FollowerEsp01/FollowerBus.cpp").read_text()


def body(source: str, signature: str) -> str:
    start = source.index(signature)
    return source[start:source.index("\n}\n", start)]


def test_master_home_shows_the_frame_again():
    assert "reshowLastFrame(local)" in body(MASTER, "static void execHome(")


def test_master_self_test_shows_the_frame_again():
    assert "reshowLastFrame(local)" in body(MASTER, "static void execSelfTest(")


def test_master_scan_after_a_bootloader_window_shows_the_frame_again():
    tick = body(MASTER, "static void heartbeatTick(")
    owed = tick[tick.index("if (probeOwedAfterRiskWindow) {"):]
    owed = owed[:owed.index("return;")]
    assert owed.index("unitBusProbe(") < owed.index("reshowLastFrame(local)")


def test_master_reshow_goes_through_the_one_helper():
    helper = body(MASTER, "static void reshowLastFrame(")
    assert "lastFrameValid" in helper and "unitBusShowFrame(" in helper


def test_master_reshow_writes_nothing_inside_a_bootloader_window():
    helper = body(MASTER, "static void reshowLastFrame(")
    window = helper.index("twibootRiskUntilMs - millis()")
    assert window < helper.index("probeOwedAfterRiskWindow = true") < helper.index("unitBusShowFrame(")


def test_both_boards_ask_the_shared_rule_after_a_self_test():
    assert "selfTestMovedTheDrum(slot.outcome)) reshowLastFrame(local)" in MASTER
    assert "selfTestMovedTheDrum(outcome)) busReshowLastFrame()" in ROW_JOBS


def test_row_home_shows_the_frame_again():
    """Once the search has ended: the show waits for the row to stand still."""
    assert "busReshowLastFrame()" in body(ROW_JOBS, "static void pollHome(")


def test_row_self_test_shows_the_frame_again():
    assert "busReshowLastFrame()" in body(ROW_JOBS, "static void pollSelfTest(")


def test_row_scan_after_a_bootloader_window_shows_the_frame_again():
    tick = body(ROW_JOBS, "void unitJobsLoopTick(")
    owed = tick[tick.index("if (unitHealthRefreshPending) {"):]
    assert re.search(r"busProbeInhibitedUntilMs\(\).*busProbe\(\);.*busReshowLastFrame\(\);", owed, re.S)


def test_row_reshow_is_staged_not_run_in_the_job():
    assert re.search(r"void busReshowLastFrame\(\) \{ reshowPending = lastFrameValid; \}", ROW_BUS)


def test_master_frames_wait_out_a_bootloader_window():
    """Every frame the master writes outside a unit update waits the window out (#581)."""
    wait = body(MASTER, "static int showFrameOutsideBootloaderWindow(")
    assert wait.index("twibootRiskUntilMs - millis()") < wait.index("unitBusShowFrame(")
    raw = [m.start() for m in re.finditer(r"unitBusShowFrame\(", MASTER)]
    # The wait itself, the re-show (which defers instead), and the two re-shows
    # inside the unit update and bootloader update jobs, which own the window.
    assert len(raw) == 4, len(raw)
    for name in ("execShowText", "execResetUnits", "execStop", "execReflashUnits"):
        assert "showFrameOutsideBootloaderWindow(" in body(MASTER, f"static void {name}(")
