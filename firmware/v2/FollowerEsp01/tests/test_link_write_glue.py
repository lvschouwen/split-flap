"""Source gate: what the row writes to its master without being asked
(vitals, unit facts, log lines, events) must never wait for the master, and a
connection must not be closed over one late acknowledgement. Seen on the
wall: with every write waiting at most 1 s, the link closed and was dialled
again every 68 s for seven hours. The rules themselves are
FollowerLinkPolicy.h, natively tested."""
import re
from pathlib import Path

LINK = Path(__file__).resolve().parent.parent / "FollowerLink.cpp"


def _code() -> str:
    return re.sub(r"//[^\n]*", "", LINK.read_text())


def _body(code: str, signature: str) -> str:
    body = code[code.index(signature):]
    return body[:body.index("\n}\n")]


def test_what_can_wait_is_written_only_when_the_buffer_has_room() -> None:
    code = _code()
    for signature in ("void sendStatus(bool timeSynced) {", "void unitsTick() {",
                      "void logTick() {", "void eventsTick() {"):
        body = _body(code, signature)
        assert "sendWhenRoom()" in body, signature
        assert not re.search(r"\bsend\(\)", body), signature
    room = _body(code, "bool sendWhenRoom() {")
    assert "followerLinkRoomFor((size_t)sock.availableForWrite(), n)" in room


def test_a_write_has_its_own_wait_apart_from_the_connect() -> None:
    code = _code()
    connect = code.index("sock.connect(host, WALL_LINK_PORT)")
    assert code.rindex("sock.setTimeout(FOLLOWER_LINK_CONNECT_TIMEOUT_MS);", 0, connect) > 0
    assert code.index("sock.setTimeout(FOLLOWER_LINK_WRITE_TIMEOUT_MS);", connect) > connect


def test_a_silent_master_is_dialled_again_and_a_busy_row_does_not_count() -> None:
    code = _code()
    tick = code[code.index("void linkLoopTick() {"):]
    assert "followerLinkHeardAfterPass(lastHeardMs, lastPassMs, passMs, unitOpsBusy())" in tick
    silent = tick.index("if (followerLinkMasterSilent(millis(), lastHeardMs)) {")
    assert tick.index("int avail = sock.available();") < silent < tick.index("opTick();")
    assert "lastHeardMs = millis();" in _body(code, "void handle(const wl_ToRow& m, const FollowerClusterView& view) {")


def test_a_close_is_logged_with_its_facts_and_a_run_of_them_is_damped() -> None:
    code = _code()
    closed = _body(code, "void logClosed(const __FlashStringHelper* why) {")
    assert closed.index("wlLogDampWrite(closeDamp, nowMs)") < closed.index("SerialPrint(why);")
    for fact in ("connectedAtMs", "sock.availableForWrite()", "lastHeardMs", "WiFi.RSSI()",
                 "WiFi.BSSIDstr()"):
        assert fact in closed, fact
    assert "logClosed(why);" in _body(code, "void drop(const __FlashStringHelper* why) {")
