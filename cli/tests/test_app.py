import threading

import httpx
import pytest
from splitflap_client.events import DisplayEvent
from splitflap_client.models import ClusterStatus, StatusAggregate
from splitflap_client.transport import BoardClient
from splitflap_tui.app import SplitflapApp
from splitflap_tui.commands import parse
from splitflap_tui.confirm import ConfirmModal
from splitflap_tui.config import Board, Config
from splitflap_tui.widgets import ClusterStrip, LogTail, UnitsTable, WallPanel

STATUS = {"settings": {"plat": "esp32s3", "unitCount": 16, "version": "817e3a9",
                       "clusterLeading": True, "deviceMode": "clock"},
          "stats": {"now": {"rssi": -52, "heap": 180000, "minHeap": 150000,
                            "uptime": 3600, "hwm": {"cluster": 2600}}},
          "units": {"width": 16, "faulty": 0,
                    "units": [{"i": 0, "a": 1, "st": 1, "v": 1, "sx": 17}]},
          "cluster": {"enabled": True, "epoch": 7, "seq": 1,
                      "members": [{"host": "", "self": True, "row": 1, "col": 0,
                                   "width": 16, "joined": True,
                                   "degraded": False, "failures": 0,
                                   "rev": "817e3a9", "hmac": True}],
                      "rollout": {"phase": "idle"},
                      "followerImage": {"present": False, "rev": ""},
                      "followerPush": {"phase": "idle", "result": "none"}},
          "ota": {"running": "app0", "next": "app1", "lastInvalid": None,
                  "lastFlashResult": "", "otaReverted": False,
                  "factoryValid": True}}


def fake_factory(url: str) -> BoardClient:
    def handler(req):
        if req.url.path == "/status":
            return httpx.Response(200, json=STATUS)
        if req.url.path == "/cluster/status":
            return httpx.Response(200, json=STATUS["cluster"])
        if req.url.path == "/log/flash":
            return httpx.Response(200, text="hello log\n")
        if req.url.path == "/events":
            return httpx.Response(200, content=b"",
                                  headers={"content-type": "text/event-stream"})
        return httpx.Response(404, text="nope")
    return BoardClient(url, transport=httpx.MockTransport(handler))


CFG = Config(boards=[Board("leader", "http://leader")], default="leader")


@pytest.mark.asyncio
async def test_dashboard_shows_polled_data():
    app = SplitflapApp(CFG, client_factory=fake_factory)
    async with app.run_test() as pilot:
        await pilot.pause(0.5)
        strip = app.query_one("#cluster-strip")
        assert "817e3a9" in strip.cluster_text()


@pytest.mark.asyncio
async def test_wall_marks_stale_when_sse_down():
    app = SplitflapApp(CFG, client_factory=fake_factory)
    async with app.run_test() as pilot:
        await pilot.pause(0.5)
        assert app.wall_stale is True     # empty SSE stream ended -> stale
        # #441 follow-up: apply_wall_stale's border_title assignment must
        # survive Textual's markup parsing — a raw "wall [STALE]" string
        # gets its unclosed "[STALE]" tag silently swallowed, same bug the
        # panel-staleness widgets were fixed for (finding 5).
        wall = app.query_one("#wall", WallPanel)
        assert "STALE" in wall.border_title


# ---- #441 finding 1: board-supplied display text must render literally,
# never be parsed as Rich console markup — "[/]" is valid board text (an
# auto-closing tag with nothing to close used to raise MarkupError inside
# call_from_thread, which escaped poller.py's old narrow `except
# SplitflapError` and permanently killed the SSE thread).

class _ScriptedSseStream(httpx.SyncByteStream):
    """A persistent SSE body that yields one chunk per gate, blocking
    between chunks until the test releases the next one — models a real
    board's /events connection staying open, unlike a MockTransport's
    default fully-buffered (and thus instantly-EOF) response body."""

    def __init__(self, chunks: list[bytes], gates: list[threading.Event]):
        self._chunks = chunks
        self._gates = gates

    def __iter__(self):
        for chunk, gate in zip(self._chunks, self._gates):
            yield chunk
            gate.wait(5.0)

    def close(self) -> None:
        pass


@pytest.mark.asyncio
async def test_sse_malformed_and_bracketed_text_render_literally_not_stale():
    malformed_gate = threading.Event()
    done_gate = threading.Event()
    chunk_malformed = b'event: display\ndata: {"text": "[/][/] MALFORMED"}\n\n'
    chunk_legit = b'event: display\ndata: {"text": "[ICE 704]"}\n\n'

    def handler(req):
        if req.url.path == "/events":
            stream = _ScriptedSseStream([chunk_malformed, chunk_legit],
                                        [malformed_gate, done_gate])
            return httpx.Response(200, headers={"content-type": "text/event-stream"},
                                  stream=stream)
        if req.url.path == "/status":
            return httpx.Response(200, json=STATUS)
        if req.url.path == "/cluster/status":
            return httpx.Response(200, json=STATUS["cluster"])
        if req.url.path == "/log/flash":
            return httpx.Response(200, text="hello log\n")
        return httpx.Response(404, text="nope")

    factory = lambda url: BoardClient(url, transport=httpx.MockTransport(handler))
    app = SplitflapApp(CFG, client_factory=factory)
    async with app.run_test() as pilot:
        await pilot.pause(0.3)      # sse_loop connects and gets chunk 1
        wall = app.query_one("#wall", WallPanel)
        # Task 3 (#450): cells interleave ▐▌ glyphs into rendered content, so
        # the literal-rendering invariant is now pinned on the logical text
        # accessor instead of the visual content.
        assert "[/][/] MALFORMED" in wall.wall_text()  # literal, not parsed
        assert app.wall_stale is False
        assert app.is_running                          # thread didn't die

        malformed_gate.set()        # release chunk 2
        await pilot.pause(0.3)
        assert "[ICE 704]" in wall.wall_text()
        assert app.wall_stale is False

        done_gate.set()              # let the stream end so teardown is clean
        await pilot.pause(0.1)


@pytest.mark.asyncio
async def test_stale_panels_get_css_class_and_recover():
    app = SplitflapApp(CFG, client_factory=fake_factory)
    async with app.run_test() as pilot:
        await pilot.pause(0.3)
        strip = app.query_one("#cluster-strip", ClusterStrip)
        units = app.query_one("#units", UnitsTable)
        log = app.query_one("#log", LogTail)
        wall = app.query_one("#wall", WallPanel)

        app.apply_disconnect("boom")
        app.apply_wall_stale()
        await pilot.pause(0.05)
        assert strip.has_class("stale") and units.has_class("stale")
        assert log.has_class("stale") and wall.has_class("stale")

        agg = StatusAggregate.from_json(STATUS)
        app.apply_status(agg, ClusterStatus.from_json(STATUS["cluster"]))
        app.apply_display(DisplayEvent(text="OK", self_row=None, rows=None))
        await pilot.pause(0.05)
        assert not strip.has_class("stale") and not units.has_class("stale")
        assert not log.has_class("stale") and not wall.has_class("stale")


@pytest.mark.asyncio
async def test_disconnect_marks_panels_stale_and_reconnect_clears():
    """#441 finding 5: apply_disconnect must mark ClusterStrip, UnitsTable
    and LogTail stale (not just the stats bar), and apply_status must clear
    those markers again on the next successful poll."""
    app = SplitflapApp(CFG, client_factory=fake_factory)
    async with app.run_test() as pilot:
        await pilot.pause(0.3)
        strip = app.query_one("#cluster-strip", ClusterStrip)
        units = app.query_one("#units", UnitsTable)
        log = app.query_one("#log", LogTail)

        app.apply_disconnect("boom")
        await pilot.pause(0.05)
        assert "STALE" in strip.border_title
        assert "STALE" in units.border_title
        assert "STALE" in log.border_title

        agg = StatusAggregate.from_json(STATUS)
        cluster = ClusterStatus.from_json(STATUS["cluster"])
        app.apply_status(agg, cluster)
        await pilot.pause(0.05)
        assert "STALE" not in strip.border_title
        assert "STALE" not in units.border_title
        assert "STALE" not in log.border_title


@pytest.mark.asyncio
async def test_wall_renders_flap_cells():
    app = SplitflapApp(CFG, client_factory=fake_factory)
    async with app.run_test() as pilot:
        await pilot.pause(0.3)
        wall = app.query_one("#wall", WallPanel)
        app.apply_display(DisplayEvent(text="HI", self_row=None, rows=None))
        await pilot.pause(0.05)
        assert wall.wall_text() == "HI"
        assert "▐H▌ ▐I▌" in wall.content        # cells, not a plain string


@pytest.mark.asyncio
async def test_text_in_clock_mode_asks_before_sending():
    app = SplitflapApp(CFG, client_factory=fake_factory)
    async with app.run_test() as pilot:
        await pilot.pause(0.3)                 # STATUS fixture: deviceMode=clock
        assert app.device_mode == "clock"
        app.dispatch_command(parse("text HI"))
        await pilot.pause(0.05)
        assert isinstance(app.screen, ConfirmModal)
        assert "clock" in app.screen.summary
        app.screen.action_no()                 # cancel; nothing sent
        await pilot.pause(0.05)


@pytest.mark.asyncio
async def test_text_outside_clock_mode_sends_directly():
    app = SplitflapApp(CFG, client_factory=fake_factory)
    async with app.run_test() as pilot:
        await pilot.pause(0.3)
        for mode in ("text", ""):              # explicit text mode + unknown
            app.device_mode = mode
            app.dispatch_command(parse("text HI"))
            await pilot.pause(0.1)
            assert not isinstance(app.screen, ConfirmModal)
