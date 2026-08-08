import httpx
import pytest
from splitflap_client.control import (notify, post_form, reboot, set_mode,
                                      set_setting, set_text, stop)
from splitflap_client.transport import BoardClient, HttpError


def capture():
    seen = {}
    def handler(req):
        seen["path"] = req.url.path
        seen["form"] = dict(httpx.QueryParams(req.content.decode()))
        return httpx.Response(200, text="ok")
    return seen, BoardClient("http://b", transport=httpx.MockTransport(handler))


def test_ajax_always_set_and_text_field():
    seen, c = capture()
    assert set_text(c, "TRAINS LATE") == "ok"
    assert seen["path"] == "/" and seen["form"]["ajax"] == "1"
    assert seen["form"]["inputText"] == "TRAINS LATE"


def test_notify_sends_transient_pair():
    seen, c = capture()
    notify(c, "DOOR", 15)
    assert seen["form"]["transientText"] == "DOOR"
    assert seen["form"]["transientDwell"] == "15"


def test_mode_field():
    seen, c = capture()
    set_mode(c, "clock")
    assert seen["form"]["deviceMode"] == "clock"


def test_clustered_409_verbatim():
    c = BoardClient("http://b", transport=httpx.MockTransport(
        lambda r: httpx.Response(409, text="clustered")))
    with pytest.raises(HttpError) as exc:
        set_text(c, "X")
    assert exc.value.body == "clustered"


def test_stop_returns_seq():
    c = BoardClient("http://b", transport=httpx.MockTransport(
        lambda r: httpx.Response(200, json={"seq": 9})))
    assert stop(c) == 9


def test_reboot_posts():
    seen = {}
    def handler(req):
        seen["path"] = req.url.path
        return httpx.Response(200, text="rebooting")
    reboot(BoardClient("http://b", transport=httpx.MockTransport(handler)))
    assert seen["path"] == "/reboot"


def test_set_setting_passes_field_through_with_ajax():
    seen, c = capture()
    set_setting(c, "flapSpeed", 42)
    assert seen["form"]["flapSpeed"] == "42"
    assert seen["form"]["ajax"] == "1"
