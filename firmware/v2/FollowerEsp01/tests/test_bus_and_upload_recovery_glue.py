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
    reprobe = tick[tick.index("busRecoveryReprobeDue(busRecovery, detectedUnitCount)"):]
    assert "busRecoveryNoteReprobe(busRecovery, detectedUnitCount);" in reprobe
    closes = re.findall(r"if \(([^)]*)\) \{\s*observeLiveness\(0, true\);", reprobe)
    assert closes == ["unitReplied"], closes
    assert reprobe.count("observeLiveness(0, true)") == 1
