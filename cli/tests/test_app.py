import httpx
import pytest
from splitflap_client.transport import BoardClient
from splitflap_tui.app import SplitflapApp
from splitflap_tui.config import Board, Config

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
        assert "817e3a9" in strip.render_str()


@pytest.mark.asyncio
async def test_wall_marks_stale_when_sse_down():
    app = SplitflapApp(CFG, client_factory=fake_factory)
    async with app.run_test() as pilot:
        await pilot.pause(0.5)
        assert app.wall_stale is True     # empty SSE stream ended -> stale
