import httpx
import pytest
from splitflap_client.transport import BoardClient, HttpError, ParseError, Unreachable


def make_client(handler):
    return BoardClient("http://board", transport=httpx.MockTransport(handler))


def test_get_json_parses_body():
    client = make_client(lambda req: httpx.Response(200, json={"plat": "esp32s3"}))
    assert client.get_json("/settings") == {"plat": "esp32s3"}


def test_http_error_carries_verbatim_body_and_status():
    body = "Unit reflash in progress — retry when it finishes"
    client = make_client(lambda req: httpx.Response(409, text=body))
    with pytest.raises(HttpError) as exc:
        client.post("/unit/home", params={"address": 3})
    assert exc.value.status == 409
    assert exc.value.body == body


def test_transport_error_wraps_as_unreachable():
    def handler(req):
        raise httpx.ConnectError("refused")
    client = make_client(handler)
    with pytest.raises(Unreachable):
        client.get_json("/settings")


def test_invalid_json_is_parse_error():
    client = make_client(lambda req: httpx.Response(200, text="not json"))
    with pytest.raises(ParseError):
        client.get_json("/settings")


def test_post_sends_form_data():
    seen = {}
    def handler(req):
        seen["content"] = req.content.decode()
        return httpx.Response(200, text="ok")
    client = make_client(handler)
    client.post("/", data={"inputText": "HELLO", "ajax": "1"})
    assert "inputText=HELLO" in seen["content"] and "ajax=1" in seen["content"]
