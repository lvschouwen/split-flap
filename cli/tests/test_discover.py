import httpx
import pytest
from splitflap_client.capability import PLAT_ESP01, PLAT_S3
from splitflap_client.discover import (DiscoverTimeout, DiscoveredBoard,
                                       parse_discover, run_discover)
from splitflap_client.transport import BoardClient, HttpError, ParseError


def make_client(handler):
    return BoardClient("http://b", transport=httpx.MockTransport(handler))


# The 200 body's exact shape (buildClusterDiscoverJson, ClusterDiscovery.h:63):
# the board's own advertisement is already filtered out firmware-side.
DONE = {"status": "done", "boards": [
    {"name": "wall-row0", "host": "192.168.15.121", "rev": "bb958fb",
     "width": 5, "plat": "esp01"},
    {"name": "spare", "host": "spare.local", "rev": "941d8a9", "width": 16},
]}


def started(request):
    return httpx.Response(200, text="Board discovery started")


def test_parse_discover_reads_both_plat_shapes():
    boards = parse_discover(DONE)
    assert boards[0] == DiscoveredBoard("wall-row0", "192.168.15.121",
                                        "bb958fb", 5, PLAT_ESP01)
    # #297 additive: only foreign-platform boards advertise a plat tag
    # (ClusterDiscovery.h:69) — an absent one means an S3, the same
    # inference plat_from_settings makes.
    assert boards[1].plat == PLAT_S3


def test_parse_discover_tolerates_missing_mistyped_and_extra_fields():
    payload = {"status": "done", "boards": [
        {},                                       # every field absent
        {"name": "x", "width": "16"},             # width a string, not a number
        "not-a-board",                            # not even a dict
        {"name": "y", "host": "h", "future": 1},  # additive key from newer fw
    ]}
    boards = parse_discover(payload)
    assert [b.name for b in boards] == ["", "x", "y"]
    assert boards[1].width == 0        # mistyped -> default, never a crash
    assert boards[2].host == "h"


def test_parse_discover_non_list_boards_is_empty():
    assert parse_discover({"status": "done"}) == []
    assert parse_discover({"boards": "nope"}) == []
    assert parse_discover([]) == []


def test_run_discover_posts_then_polls_until_done():
    seen = []

    def handler(req):
        seen.append((req.method, req.url.path))
        if req.method == "POST":
            return started(req)
        if sum(1 for m, _ in seen if m == "GET") < 3:
            return httpx.Response(202, text="Discovery running")
        return httpx.Response(200, json=DONE)

    naps = []
    boards = run_discover(make_client(handler), sleep=naps.append,
                          clock=lambda: len(naps) * 0.5)
    assert [b.name for b in boards] == ["wall-row0", "spare"]
    assert seen[0] == ("POST", "/cluster/discover")
    assert naps == [0.5, 0.5]          # slept between the two 202s only


def test_run_discover_returns_immediately_when_already_done():
    def handler(req):
        return started(req) if req.method == "POST" \
            else httpx.Response(200, json=DONE)

    naps = []
    boards = run_discover(make_client(handler), sleep=naps.append,
                          clock=lambda: 0.0)
    assert len(boards) == 2 and naps == []


def test_run_discover_empty_result_is_a_normal_empty_list():
    """A scan that finds nothing is the normal answer across a VPN — an
    empty boards array must not read as an error."""
    def handler(req):
        return started(req) if req.method == "POST" \
            else httpx.Response(200, json={"status": "done", "boards": []})

    assert run_discover(make_client(handler), sleep=lambda s: None,
                        clock=lambda: 0.0) == []


def test_run_discover_times_out_while_still_running():
    def handler(req):
        return started(req) if req.method == "POST" \
            else httpx.Response(202, text="Discovery running")

    ticks = iter([0.0, 0.0, 5.0, 11.0])
    with pytest.raises(DiscoverTimeout) as exc:
        run_discover(make_client(handler), sleep=lambda s: None,
                     clock=lambda: next(ticks))
    assert "10" in str(exc.value)


def test_run_discover_propagates_404_verbatim():
    body = "No discovery has run yet"

    def handler(req):
        return started(req) if req.method == "POST" \
            else httpx.Response(404, text=body)

    with pytest.raises(HttpError) as exc:
        run_discover(make_client(handler), sleep=lambda s: None,
                     clock=lambda: 0.0)
    assert exc.value.body == body


def test_run_discover_post_error_propagates_and_never_polls():
    seen = []

    def handler(req):
        seen.append(req.method)
        return httpx.Response(503, text="busy")

    with pytest.raises(HttpError):
        run_discover(make_client(handler), sleep=lambda s: None,
                     clock=lambda: 0.0)
    assert seen == ["POST"]


def test_run_discover_malformed_json_raises_parse_error():
    def handler(req):
        return started(req) if req.method == "POST" \
            else httpx.Response(200, text="<html>not json</html>")

    with pytest.raises(ParseError):
        run_discover(make_client(handler), sleep=lambda s: None,
                     clock=lambda: 0.0)
