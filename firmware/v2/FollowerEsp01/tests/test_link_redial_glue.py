"""Source gate: a row whose master closes the connection before Welcome (a
master that does not know it) must wait out the backoff before it dials
again. Seen on the wall: without it the row dialled about twenty times a
second. The backoff rule itself is FollowerLinkPolicy.h, natively tested."""
import re
from pathlib import Path

LINK = Path(__file__).resolve().parent.parent / "FollowerLink.cpp"


def _code() -> str:
    return re.sub(r"//[^\n]*", "", LINK.read_text())


def test_a_connection_closed_before_welcome_is_dropped_before_the_next_dial() -> None:
    code = _code()
    closed = code[code.index("if (!sock.connected()) {"):]
    dropped = closed.index("if (dialled) drop(")
    assert dropped < closed.index("if ((int32_t)(millis() - nextDialMs) < 0) return;")
    assert dropped < closed.index("sock.connect(")


def test_every_opened_connection_is_marked_and_every_drop_clears_the_mark() -> None:
    code = _code()
    assert code.index("dialled = true;") < code.index("sendHello();", code.index("dialled = true;"))
    drop = code[code.index("void drop(const __FlashStringHelper* why) {"):]
    drop = drop[:drop.index("\n}\n")]
    assert "dialled = false;" in drop
    assert "backoffMs = followerLinkNextBackoffMs(backoffMs);" in drop
