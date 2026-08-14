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


def test_stream_sends_sse_accept_header():
    seen = {}
    def handler(req):
        seen["accept"] = req.headers.get("accept")
        return httpx.Response(200, content=b"",
                              headers={"content-type": "text/event-stream"})
    client = make_client(handler)
    with client.stream("/events") as resp:
        resp.read()
    assert seen["accept"] == "text/event-stream"


def test_get_returns_the_response_so_a_202_survives():
    """get_json can't read GET /cluster/discover: while the leader's mDNS
    browse runs it answers 202 with a text/plain body, and only 200 carries
    JSON. get() hands the caller the status to branch on."""
    client = make_client(lambda r: httpx.Response(202, text="Discovery running"))
    resp = client.get("/cluster/discover")
    assert resp.status_code == 202 and resp.text == "Discovery running"


def test_get_still_raises_on_error_status():
    client = make_client(lambda r: httpx.Response(404, text="No discovery has run yet"))
    with pytest.raises(HttpError) as exc:
        client.get("/cluster/discover")
    assert exc.value.body == "No discovery has run yet"
