import asyncio
import threading

import httpx
import pytest
from splitflap_client.transport import BoardClient
from splitflap_tui.app import SplitflapApp
from splitflap_tui.config import Board, Config
from splitflap_tui.screens import board_detail
from splitflap_tui.screens.board_detail import BoardDetailScreen
from splitflap_tui.screens.help_screen import HelpScreen
from splitflap_tui.screens.log_screen import LogScreen
from textual.widgets import RichLog

from tests.test_app import CFG, fake_factory

ESP01_SETTINGS = {"plat": "esp01", "width": 5, "version": "9f694dd",
                  "clusterState": "clustered", "effectiveDeviceName": "row0"}
CLUSTER_HEALTH = {"state": "clustered", "rev": "9f694dd", "hmac": True,
                  "foreign": {"joins": 0, "pings": 0, "renders": 0,
                              "lastHost": "", "msSince": -1}}


@pytest.mark.asyncio
async def test_board_detail_esp01_polls_only_while_open():
    calls = []
    def handler(req):
        calls.append(req.url.path)
        if req.url.path == "/settings":
            return httpx.Response(200, json=ESP01_SETTINGS)
        if req.url.path == "/units/health":
            return httpx.Response(200, json={"width": 5, "faulty": 0, "units": []})
        if req.url.path == "/cluster/health":
            return httpx.Response(200, json={"state": "clustered", "rev": "9f694dd",
                                             "hmac": True,
                                             "foreign": {"joins": 0, "pings": 0,
                                                         "renders": 0,
                                                         "lastHost": "",
                                                         "msSince": -1}})
        if req.url.path == "/log":
            return httpx.Response(200, text="10\nhello\n")
        return httpx.Response(404, text="nope")
    factory = lambda url: BoardClient(url, transport=httpx.MockTransport(handler))
    # default="" (not "row0"): keeps this test hermetic to BoardDetailScreen's
    # own poll thread. With default == the only board's name, SplitflapApp's
    # on_mount() would ALSO start its own dashboard Poller against the same
    # mock URL, whose SSE reconnect loop (1 s initial backoff) fires a second
    # /events request inside the post-escape pause window below and pollutes
    # `calls` with a call this test isn't about.
    cfg = Config(boards=[Board("row0", "http://row0")], default="")
    app = SplitflapApp(cfg, client_factory=factory)
    async with app.run_test() as pilot:
        app.push_screen(BoardDetailScreen(cfg.boards[0], factory))
        await pilot.pause(0.5)
        assert "/settings" in calls and "/log" in calls
        n_before = len(calls)
        await pilot.press("escape")
        await pilot.pause(0.6)
        assert len(calls) == n_before       # no polling after close


@pytest.mark.asyncio
async def test_board_detail_follower_log_cursor_advances(monkeypatch):
    # POLL_S lowered from the production 5 s floor purely to keep this
    # test's runtime short — it needs >=2 poll cycles to observe the
    # cursor advance from one reply to the next request's `after` param.
    # The 5 s floor itself is exercised for real by the sibling test above
    # (and by the live smoke in the task report); this test only pins the
    # cursor arithmetic in board_detail._poll.
    monkeypatch.setattr(board_detail, "POLL_S", 0.2)
    log_afters: list[str | None] = []
    log_call_count = 0

    def handler(req):
        nonlocal log_call_count
        if req.url.path == "/settings":
            return httpx.Response(200, json=ESP01_SETTINGS)
        if req.url.path == "/units/health":
            return httpx.Response(200, json={"width": 5, "faulty": 0, "units": []})
        if req.url.path == "/cluster/health":
            return httpx.Response(200, json=CLUSTER_HEALTH)
        if req.url.path == "/log":
            log_afters.append(req.url.params.get("after"))
            log_call_count += 1
            # first reply's cursor is 10; every reply after that is 20 —
            # the assertion only needs the SECOND request to carry after=10.
            if log_call_count == 1:
                return httpx.Response(200, text="10\nhello\n")
            return httpx.Response(200, text="20\nworld\n")
        return httpx.Response(404, text="nope")

    factory = lambda url: BoardClient(url, transport=httpx.MockTransport(handler))
    cfg = Config(boards=[Board("row0", "http://row0")], default="")
    app = SplitflapApp(cfg, client_factory=factory)
    async with app.run_test() as pilot:
        app.push_screen(BoardDetailScreen(cfg.boards[0], factory))
        await pilot.pause(0.7)          # several 0.2 s cycles
        assert len(log_afters) >= 2
        assert log_afters[0] == "0"     # initial cursor
        assert log_afters[1] == "10"    # advanced from the first reply's cursor
        await pilot.press("escape")
        await pilot.pause(0.1)


@pytest.mark.asyncio
async def test_log_screen_renders_and_prev_toggle_refetches():
    calls: list[dict] = []

    def handler(req):
        if req.url.path == "/log/flash":
            calls.append(dict(req.url.params))
            return httpx.Response(200, text="line1\nline2\n")
        return httpx.Response(404, text="nope")

    factory = lambda url: BoardClient(url, transport=httpx.MockTransport(handler))
    cfg = Config(boards=[Board("leader", "http://leader")], default="")
    app = SplitflapApp(cfg, client_factory=factory)
    async with app.run_test() as pilot:
        app.push_screen(LogScreen("http://leader", factory))
        await pilot.pause(0.3)
        log = app.screen.query_one("#flash-log", RichLog)
        rendered = "\n".join(strip.text for strip in log.lines)
        assert "line1" in rendered and "line2" in rendered
        assert len(calls) == 1 and "prev" not in calls[0]

        await pilot.press("p")
        await pilot.pause(0.3)
        assert len(calls) == 2 and calls[1].get("prev") == "1"

        await pilot.press("escape")
        await pilot.pause(0.1)
        assert not isinstance(app.screen, LogScreen)


@pytest.mark.asyncio
async def test_log_screen_toggle_race_last_toggle_wins():
    """#441 finding 6: consecutive `p` toggles spawn overlapping fetch
    workers — a slow FIRST fetch resolving AFTER a fast SECOND one must not
    overwrite the screen with the stale result. Here the initial (on_mount)
    fetch is held open until after the `p` toggle's fetch has already
    completed and rendered, then released — its result must be dropped."""
    initial_entered = threading.Event()
    release_initial = threading.Event()

    def handler(req):
        prev = req.url.params.get("prev")
        if prev is None:
            initial_entered.set()
            assert release_initial.wait(5.0), "test never released the initial fetch"
            return httpx.Response(200, text="INITIAL\n")
        return httpx.Response(200, text="PREV\n")

    factory = lambda url: BoardClient(url, transport=httpx.MockTransport(handler))
    cfg = Config(boards=[Board("leader", "http://leader")], default="")
    app = SplitflapApp(cfg, client_factory=factory)
    async with app.run_test() as pilot:
        app.push_screen(LogScreen("http://leader", factory))
        # a blocking .wait() here would stall the app's own event-loop
        # thread (the same thread this coroutine runs on) — hop to a
        # worker thread for the wait instead.
        entered = await asyncio.to_thread(initial_entered.wait, 2.0)
        assert entered, "initial fetch never started"

        await pilot.press("p")          # second, faster fetch — gen 2
        await pilot.pause(0.3)
        log = app.screen.query_one("#flash-log", RichLog)
        rendered = "\n".join(strip.text for strip in log.lines)
        assert "PREV" in rendered

        release_initial.set()           # let the stale gen-1 fetch resolve
        await pilot.pause(0.3)
        rendered = "\n".join(strip.text for strip in log.lines)
        assert "INITIAL" not in rendered, "stale fetch overwrote the newer toggle"
        assert "PREV" in rendered

        await pilot.press("escape")
        await pilot.pause(0.1)


@pytest.mark.asyncio
async def test_log_screen_shows_error_on_unreachable():
    def handler(req):
        return httpx.Response(503, text="board busy")

    factory = lambda url: BoardClient(url, transport=httpx.MockTransport(handler))
    cfg = Config(boards=[Board("leader", "http://leader")], default="")
    app = SplitflapApp(cfg, client_factory=factory)
    async with app.run_test() as pilot:
        app.push_screen(LogScreen("http://leader", factory))
        await pilot.pause(0.3)
        log = app.screen.query_one("#flash-log", RichLog)
        rendered = "\n".join(strip.text for strip in log.lines)
        assert "UNREACHABLE" in rendered


@pytest.mark.asyncio
async def test_question_mark_opens_help_and_escape_closes():
    app = SplitflapApp(CFG, client_factory=fake_factory)
    async with app.run_test() as pilot:
        await pilot.pause(0.3)
        await pilot.press("question_mark")
        assert isinstance(app.screen, HelpScreen)
        await pilot.press("escape")
        await pilot.pause(0.05)
        assert not isinstance(app.screen, HelpScreen)
