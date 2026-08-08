import httpx
import pytest
from splitflap_client.logs import (FollowerLogDelta, fetch_flash_log,
                                   fetch_follower_log)
from splitflap_client.transport import BoardClient, HttpError


def make_client(handler):
    return BoardClient("http://b", transport=httpx.MockTransport(handler))


def test_flash_log_plain_and_prev_param():
    seen = {}
    def handler(req):
        seen["params"] = dict(req.url.params)
        return httpx.Response(200, text="line1\nline2\n")
    c = make_client(handler)
    assert fetch_flash_log(c) == "line1\nline2\n"
    assert seen["params"] == {}
    fetch_flash_log(c, prev=True)
    assert "prev" in seen["params"]


def test_flash_log_404_is_empty_not_error():
    c = make_client(lambda r: httpx.Response(404, text="No flash log yet"))
    assert fetch_flash_log(c) == ""


def test_flash_log_503_raises():
    c = make_client(lambda r: httpx.Response(503, text="Flash log unavailable"))
    with pytest.raises(HttpError):
        fetch_flash_log(c)


def test_follower_log_cursor_parse():
    def handler(req):
        assert req.url.params["after"] == "100"
        return httpx.Response(200, text="164\nboot ok\njoined leader\n")
    d = fetch_follower_log(make_client(handler), after=100)
    assert d == FollowerLogDelta(cursor=164, text="boot ok\njoined leader\n")


def test_follower_log_empty_delta():
    d = fetch_follower_log(make_client(lambda r: httpx.Response(200, text="164\n")))
    assert d.cursor == 164 and d.text == ""
