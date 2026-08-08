import httpx
import pytest
from splitflap_client.transport import BoardClient
from splitflap_tui.app import SplitflapApp
from splitflap_tui.config import Board, Config
from splitflap_tui.screens.board_detail import BoardDetailScreen

ESP01_SETTINGS = {"plat": "esp01", "width": 5, "version": "9f694dd",
                  "clusterState": "clustered", "effectiveDeviceName": "row0"}


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
