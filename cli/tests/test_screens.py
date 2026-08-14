import asyncio
import json
import threading

import httpx
import pytest
from splitflap_client.models import ClusterStatus, StatusAggregate
from splitflap_client.transport import BoardClient
from splitflap_tui.app import SplitflapApp
from splitflap_tui.config import Board, Config
from splitflap_tui.screens import board_detail
from splitflap_tui.screens.board_detail import BoardDetailScreen
from splitflap_tui.screens.discover_screen import DiscoverScreen
from splitflap_tui.screens.health_screen import HealthScreen
from splitflap_tui.screens.help_screen import HelpScreen
from splitflap_tui.screens.log_screen import LogScreen
from textual.coordinate import Coordinate
from textual.widgets import DataTable, RichLog, Static

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


DISCOVER_DONE = {"status": "done", "boards": [
    {"name": "wall-row0", "host": "192.168.15.121", "rev": "bb958fb",
     "width": 5, "plat": "esp01"},
    {"name": "spare", "host": "spare.local", "rev": "941d8a9", "width": 16},
]}


def discover_handler(done=DISCOVER_DONE):
    def handler(req):
        if req.url.path == "/cluster/discover":
            if req.method == "POST":
                return httpx.Response(200, text="Board discovery started")
            return httpx.Response(200, json=done)
        return httpx.Response(404, text="nope")
    return handler


async def _run_discover_command(pilot, app):
    await pilot.press(":")
    await pilot.press(*"discover")
    await pilot.press("enter")
    await pilot.pause(0.4)


@pytest.mark.asyncio
async def test_discover_command_pushes_screen_and_renders_boards():
    """#469: `:discover` is routine tier — no confirm — and its result is a
    table, not a status line, so it opens its own screen."""
    factory = lambda url: BoardClient(
        url, transport=httpx.MockTransport(discover_handler()))
    cfg = Config(boards=[Board("leader", "http://leader")], default="leader")
    app = SplitflapApp(cfg, client_factory=factory)
    async with app.run_test() as pilot:
        await _run_discover_command(pilot, app)
        assert isinstance(app.screen, DiscoverScreen)
        table = app.screen.query_one("#discover-table", DataTable)
        assert table.row_count == 2
        names = [table.get_cell_at(Coordinate(r, 0)).plain
                 for r in range(table.row_count)]
        hosts = [table.get_cell_at(Coordinate(r, 1)).plain
                 for r in range(table.row_count)]
        plats = [table.get_cell_at(Coordinate(r, 4)).plain
                 for r in range(table.row_count)]
        assert names == ["wall-row0", "spare"]
        assert hosts == ["192.168.15.121", "spare.local"]
        # absent plat on the wire means an S3 (#297) — never a blank cell
        assert plats == ["esp01", "esp32s3"]
        await pilot.press("escape")
        await pilot.pause(0.1)
        assert not isinstance(app.screen, DiscoverScreen)


@pytest.mark.asyncio
async def test_discover_rejected_on_esp01_before_any_request():
    """Both /cluster/discover routes are in ESP01_NOT_SERVED — the gate must
    stop it client-side, naming the platform, with nothing on the wire."""
    calls = []

    def handler(req):
        calls.append(req.url.path)
        return httpx.Response(200, json={})

    factory = lambda url: BoardClient(url, transport=httpx.MockTransport(handler))
    cfg = Config(boards=[Board("leader", "http://leader")], default="leader")
    app = SplitflapApp(cfg, client_factory=factory)
    async with app.run_test() as pilot:
        app.plat = "esp01"
        await _run_discover_command(pilot, app)
        assert not isinstance(app.screen, DiscoverScreen)
        status = app.query_one("#cmd-status", Static)
        assert "not served on esp01" in status.content
    assert "/cluster/discover" not in calls


@pytest.mark.asyncio
async def test_discover_screen_empty_result_is_not_an_error():
    """A scan that finds nothing is the normal answer across the VPN."""
    done = {"status": "done", "boards": []}
    factory = lambda url: BoardClient(
        url, transport=httpx.MockTransport(discover_handler(done)))
    cfg = Config(boards=[Board("leader", "http://leader")], default="")
    app = SplitflapApp(cfg, client_factory=factory)
    async with app.run_test() as pilot:
        app.push_screen(DiscoverScreen("http://leader", factory))
        await pilot.pause(0.4)
        status = app.screen.query_one("#discover-status", Static)
        assert "no boards found" in status.content
        assert "⛔" not in status.content


@pytest.mark.asyncio
async def test_discover_screen_surfaces_board_error_verbatim():
    def handler(req):
        return httpx.Response(503, text="board busy")

    factory = lambda url: BoardClient(url, transport=httpx.MockTransport(handler))
    cfg = Config(boards=[Board("leader", "http://leader")], default="")
    app = SplitflapApp(cfg, client_factory=factory)
    async with app.run_test() as pilot:
        app.push_screen(DiscoverScreen("http://leader", factory))
        await pilot.pause(0.4)
        status = app.screen.query_one("#discover-status", Static)
        assert "board busy" in status.content


@pytest.mark.asyncio
async def test_discover_screen_rescan_race_last_scan_wins():
    """Same hazard LogScreen's generation guard exists for (#441 finding 6):
    a slow FIRST scan resolving AFTER a fast rescan must not overwrite the
    newer result. The initial on_mount scan is held open until the `r`
    rescan has already rendered, then released."""
    first_entered = threading.Event()
    release_first = threading.Event()
    gets = []
    lock = threading.Lock()

    def handler(req):
        if req.method == "POST":
            return httpx.Response(200, text="Board discovery started")
        with lock:
            gets.append(1)
            ordinal = len(gets)
        if ordinal == 1:
            first_entered.set()
            assert release_first.wait(5.0), "test never released the first scan"
            return httpx.Response(200, json={"status": "done", "boards": [
                {"name": "STALE", "host": "1.1.1.1", "rev": "", "width": 0}]})
        return httpx.Response(200, json={"status": "done", "boards": [
            {"name": "FRESH", "host": "2.2.2.2", "rev": "", "width": 0}]})

    factory = lambda url: BoardClient(url, transport=httpx.MockTransport(handler))
    cfg = Config(boards=[Board("leader", "http://leader")], default="")
    app = SplitflapApp(cfg, client_factory=factory)
    async with app.run_test() as pilot:
        app.push_screen(DiscoverScreen("http://leader", factory))
        entered = await asyncio.to_thread(first_entered.wait, 2.0)
        assert entered, "first scan never started"

        await pilot.press("r")              # second, faster scan — gen 2
        await pilot.pause(0.4)
        table = app.screen.query_one("#discover-table", DataTable)
        assert table.get_cell_at(Coordinate(0, 0)).plain == "FRESH"

        release_first.set()                 # let the stale gen-1 scan resolve
        await pilot.pause(0.4)
        assert table.row_count == 1
        assert table.get_cell_at(Coordinate(0, 0)).plain == "FRESH", \
            "stale scan overwrote the newer rescan"


def test_board_detail_log_buffer_is_capped():
    from splitflap_tui.screens.board_detail import LOG_CAP_LINES, cap_log
    text = "\n".join(f"line {i}" for i in range(500))
    capped = cap_log(text)
    lines = capped.splitlines()
    assert len(lines) == LOG_CAP_LINES
    assert lines[-1] == "line 499"


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


@pytest.mark.asyncio
async def test_help_table_cells_render_brackets_literally():
    # DataTable's default cell formatter markup-parses plain str cells —
    # "reboot [board]" would lose everything from "[board]" onward. The
    # help screen wraps every cell in rich.text.Text so it renders literally.
    app = SplitflapApp(CFG, client_factory=fake_factory)
    async with app.run_test() as pilot:
        await pilot.pause(0.3)
        await pilot.press("question_mark")
        assert isinstance(app.screen, HelpScreen)
        table = app.screen.query_one("#help-table", DataTable)
        cells = [table.get_cell_at(Coordinate(row, 0)) for row in range(table.row_count)]
        assert any("[board]" in c.plain for c in cells)
        assert any("[value]" in c.plain for c in cells)
        await pilot.press("escape")
        await pilot.pause(0.05)


# ---- #472: board-health screen ---------------------------------------

@pytest.mark.asyncio
async def test_s_opens_health_screen_with_polled_values():
    from tests.test_app import STATUS
    app = SplitflapApp(CFG, client_factory=fake_factory)
    async with app.run_test() as pilot:
        await pilot.pause(0.5)
        await pilot.press("s")
        await pilot.pause(0.2)
        assert isinstance(app.screen, HealthScreen)
        rendered = app.screen.query_one("#health-body", Static).content
        assert STATUS["ota"]["running"] in rendered      # app0
        assert "cluster" in rendered                     # the hwm task row
        assert "2600 B free" in rendered
        await pilot.press("escape")
        await pilot.pause(0.1)
        assert not isinstance(app.screen, HealthScreen)


@pytest.mark.asyncio
async def test_health_screen_before_any_poll_says_so():
    """No successful /status yet (unreachable board) — the screen must say
    it has nothing rather than render a page of dashes as if they were
    readings, or crash on a None aggregate."""
    def dead(url):
        return BoardClient(url, transport=httpx.MockTransport(
            lambda r: httpx.Response(503, text="down")))
    cfg = Config(boards=[Board("leader", "http://leader")], default="leader")
    app = SplitflapApp(cfg, client_factory=dead)
    async with app.run_test() as pilot:
        await pilot.pause(0.3)
        await pilot.press("s")
        await pilot.pause(0.2)
        assert isinstance(app.screen, HealthScreen)
        assert "no status yet" in app.screen.query_one("#health-body", Static).content


@pytest.mark.asyncio
async def test_health_screen_follows_later_polls():
    """The dashboard poller keeps running while a screen is pushed, so an
    open health screen must show the newest reading, not the one it was
    opened with."""
    from tests.test_app import STATUS
    app = SplitflapApp(CFG, client_factory=fake_factory)
    async with app.run_test() as pilot:
        await pilot.pause(0.5)
        await pilot.press("s")
        await pilot.pause(0.2)
        body = app.screen.query_one("#health-body", Static)
        assert "app0" in body.content
        moved = json.loads(json.dumps(STATUS))
        moved["ota"]["running"] = "app1"
        app.apply_status(StatusAggregate.from_json(moved),
                         ClusterStatus.from_json(moved["cluster"]))
        await pilot.pause(0.1)
        assert "app1" in body.content
