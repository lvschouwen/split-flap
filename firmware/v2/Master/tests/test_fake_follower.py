"""Pins the fake follower (#278) to the /cluster wire contract the real
follower implements (WebEndpoints.cpp + ClusterFollowerPolicy.h) — the
epoch/seq acceptance rules, the 409 not-clustered replies and the join
handshake shape the leader (#273) depends on."""

import json
import threading
import urllib.error
import urllib.request
from urllib.parse import urlencode

import pytest

from fake_follower import make_server


@pytest.fixture()
def follower():
    server, state = make_server(0, name="wall-2", rev="abc1234", width=16)
    port = server.server_address[1]
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    yield f"http://127.0.0.1:{port}", state
    server.shutdown()


def post(base, path, fields=None):
    data = urlencode(fields or {}).encode()
    request = urllib.request.Request(base + path, data=data, method="POST")
    try:
        with urllib.request.urlopen(request, timeout=5) as response:
            return response.status, response.read().decode()
    except urllib.error.HTTPError as error:
        return error.code, error.read().decode()


def get(base, path):
    with urllib.request.urlopen(base + path, timeout=5) as response:
        return response.status, response.read().decode()


def join(base, epoch=7, row=1):
    return post(base, "/cluster/join",
                {"leaderName": "wall-leader", "leaderHost": "192.168.15.22",
                 "row": row, "epoch": epoch})


def render(base, epoch, seq, text, speed=80, commit=0):
    return post(base, "/cluster/render",
                {"epoch": epoch, "seq": seq, "text": text, "speed": speed,
                 "commitAtMs": commit})


def test_render_before_join_is_409_not_clustered(follower):
    base, _ = follower
    status, body = render(base, 7, 1, "HELLO")
    assert status == 409
    assert json.loads(body) == {"error": "not clustered"}


def test_ping_before_join_is_409(follower):
    base, _ = follower
    status, _ = post(base, "/cluster/ping")
    assert status == 409


def test_join_reply_carries_identity_rev_width(follower):
    base, _ = follower
    status, body = join(base)
    assert status == 200
    reply = json.loads(body)
    assert reply == {"name": "wall-2", "rev": "abc1234", "width": 16,
                     "protocol": 1}


def test_join_requires_leader_host_row_epoch(follower):
    base, _ = follower
    status, _ = post(base, "/cluster/join", {"leaderName": "x"})
    assert status == 400


def test_first_render_after_join_applies(follower):
    base, state = follower
    join(base)
    status, body = render(base, 7, 1, "SEGMENT ONE     ")
    assert status == 200
    assert json.loads(body) == {"applied": True, "seq": 1}
    assert state.segment == "SEGMENT ONE     "


def test_stale_seq_is_duplicate_not_applied(follower):
    base, state = follower
    join(base)
    render(base, 7, 5, "FIVE")
    status, body = render(base, 7, 5, "FIVE AGAIN")
    assert status == 200
    assert json.loads(body)["applied"] is False
    assert state.segment == "FIVE"  # the delayed retry never regresses


def test_new_epoch_restarts_the_sequence_space(follower):
    base, state = follower
    join(base)
    render(base, 7, 50, "OLD LEADER")
    status, body = render(base, 9, 2, "REBOOTED LEADER")
    assert json.loads(body)["applied"] is True
    assert state.segment == "REBOOTED LEADER"


def test_rejoin_resets_seq_tracking(follower):
    base, _ = follower
    join(base, epoch=7)
    render(base, 7, 50, "BEFORE")
    join(base, epoch=7)  # leader re-joins after a degraded spell
    status, body = render(base, 7, 50, "RESENT")
    assert json.loads(body)["applied"] is True


def test_ping_after_join_reports_clustered_state(follower):
    base, _ = follower
    join(base, epoch=7)
    render(base, 7, 3, "X")
    status, body = post(base, "/cluster/ping")
    assert status == 200
    assert json.loads(body) == {"state": "clustered", "epoch": 7, "seq": 3}


def test_leave_returns_to_standalone(follower):
    base, _ = follower
    join(base)
    post(base, "/cluster/leave")
    status, _ = post(base, "/cluster/ping")
    assert status == 409


def test_health_reports_membership_and_segment(follower):
    base, _ = follower
    join(base, row=2)
    render(base, 7, 1, "ROW THREE       ")
    status, body = get(base, "/cluster/health")
    assert status == 200
    health = json.loads(body)
    assert health["state"] == "clustered"
    assert health["row"] == 2
    assert health["segment"] == "ROW THREE       "
    assert health["rev"] == "abc1234"


def test_applied_renders_are_recorded_for_bench_assertions(follower):
    base, state = follower
    join(base)
    render(base, 7, 1, "A", commit=1234567890123)
    render(base, 7, 1, "A-RETRY")  # duplicate — must not be recorded
    render(base, 7, 2, "B")
    texts = [r["text"] for r in state.renders]
    assert texts == ["A", "B"]
    assert state.renders[0]["commitAtMs"] == 1234567890123
