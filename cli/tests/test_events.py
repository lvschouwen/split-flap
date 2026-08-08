import httpx
import pytest
from splitflap_client.events import DisplayEvent, display_events
from splitflap_client.transport import BoardClient, Unreachable

STREAM = (b"event: display\n"
          b'data: {"text":"HELLO"}\n'
          b"\n"
          b"event: display\n"
          b'data: {"text":"WALL","selfRow":1,"rows":["ROW0","WALL"]}\n'
          b"\n")


def make_client(content=STREAM):
    def handler(req):
        return httpx.Response(200, content=content,
                              headers={"content-type": "text/event-stream"})
    return BoardClient("http://b", transport=httpx.MockTransport(handler))


def test_yields_display_events_and_wall_rows():
    events = list(display_events(make_client()))
    assert events[0] == DisplayEvent(text="HELLO", self_row=None, rows=None)
    assert events[1] == DisplayEvent(text="WALL", self_row=1,
                                     rows=["ROW0", "WALL"])


def test_ignores_unknown_fields_and_bad_json():
    content = (b"id: 123\nevent: display\ndata: not-json\n\n"
               b'event: display\ndata: {"text":"OK"}\n\n')
    events = list(display_events(make_client(content)))
    assert events == [DisplayEvent(text="OK", self_row=None, rows=None)]


def test_connect_failure_raises_unreachable():
    def handler(req):
        raise httpx.ConnectError("down")
    c = BoardClient("http://b", transport=httpx.MockTransport(handler))
    with pytest.raises(Unreachable):
        list(display_events(c))


def test_partial_event_at_eof_is_not_emitted():
    content = (b'event: display\ndata: {"text":"FIRST"}\n\n'
               b'event: display\ndata: {"text":"PARTIAL"}\n')   # no blank line
    events = list(display_events(make_client(content)))
    assert events == [DisplayEvent(text="FIRST", self_row=None, rows=None)]
