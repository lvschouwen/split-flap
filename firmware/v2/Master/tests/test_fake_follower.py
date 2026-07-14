"""Pins the fake follower (#278) to the /cluster wire contract the real
follower implements (WebEndpoints.cpp + ClusterFollowerPolicy.h) — the
epoch/seq acceptance rules, the 409 not-clustered replies, the join
handshake shape the leader (#273) depends on, and the /firmware/master
upload contract the fleet rollout (#276) streams into."""

import hashlib
import json
import threading
import time
import urllib.error
import urllib.request
from urllib.parse import urlencode

import pytest

from fake_follower import make_server

# Must byte-match ClusterRolloutPolicy.h's multipart frame — the pytest twin
# of test_multipart_frame_matches_upload_contract.
ROLLOUT_BOUNDARY = "splitflapClusterRollout"
ROLLOUT_PREAMBLE = (
    f"--{ROLLOUT_BOUNDARY}\r\n"
    'Content-Disposition: form-data; name="firmware"; '
    'filename="firmware.bin"\r\n'
    "Content-Type: application/octet-stream\r\n\r\n"
).encode()
ROLLOUT_TRAILER = f"\r\n--{ROLLOUT_BOUNDARY}--\r\n".encode()


@pytest.fixture()
def follower():
    server, state = make_server(0, name="wall-2", rev="abc1234", width=16,
                                reboot_secs=0.1)
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


def test_same_epoch_rejoin_preserves_seq_tracking(follower):
    base, _ = follower
    join(base, epoch=7)
    render(base, 7, 50, "BEFORE")
    join(base, epoch=7)  # leader re-joins after a degraded spell, no reboot
    status, body = render(base, 7, 50, "STALE RETRY")
    assert json.loads(body)["applied"] is False
    status, body = render(base, 7, 51, "FRESH RESEND")
    assert json.loads(body)["applied"] is True


def test_new_epoch_join_resets_seq_tracking(follower):
    base, _ = follower
    join(base, epoch=7)
    render(base, 7, 50, "BEFORE")
    join(base, epoch=9)  # leader rebooted
    status, body = render(base, 9, 1, "NEW EPOCH")
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


# --- fleet rollout upload contract (#276) ------------------------------------------

def upload(base, image, md5=None, v="1234abc", query=None):
    """Stream `image` exactly the way the leader's rollout does."""
    body = ROLLOUT_PREAMBLE + image + ROLLOUT_TRAILER
    if query is None:
        digest = md5 if md5 is not None else hashlib.md5(image).hexdigest()
        query = f"?md5={digest}&v={v}"
    request = urllib.request.Request(
        base + "/firmware/master" + query, data=body, method="POST",
        headers={"Content-Type":
                 f"multipart/form-data; boundary={ROLLOUT_BOUNDARY}"})
    try:
        with urllib.request.urlopen(request, timeout=5) as response:
            return response.status, response.read().decode()
    except urllib.error.HTTPError as error:
        return error.code, error.read().decode()


def wait_reboot(state, timeout=2.0):
    deadline = time.monotonic() + timeout
    while state.rebooting and time.monotonic() < deadline:
        time.sleep(0.02)
    assert not state.rebooting


def test_upload_flashes_reboots_and_adopts_leader_rev(follower):
    base, state = follower
    join(base, epoch=7)
    image = b"\xe9fake-image-bytes" * 100
    status, body = upload(base, image, v="1234abc")
    assert status == 200
    assert "rebooting" in body
    wait_reboot(state)
    assert state.rev == "1234abc"  # converged — the rejoin reports it
    assert state.epoch is None  # fresh boot: epoch/seq space forgotten
    assert state.clustered  # membership persists (NVS twin)
    assert state.uploads[0]["size"] == len(image)


def test_upload_md5_mismatch_fails_and_keeps_rev(follower):
    base, state = follower
    status, body = upload(base, b"payload", md5="0" * 32)
    assert status == 500
    assert "MD5" in body
    assert state.rev == "abc1234"
    assert not state.rebooting


def test_upload_requires_md5(follower):
    base, _ = follower
    status, _ = upload(base, b"payload", query="?v=1234abc")
    assert status == 400


def test_upload_while_busy_is_409(follower):
    base, state = follower
    post(base, "/drill/ota-busy")
    status, _ = upload(base, b"payload")
    assert status == 409
    post(base, "/drill/ota-free")
    status, _ = upload(base, b"payload")
    assert status == 200
    wait_reboot(state)


def test_rollback_drill_keeps_old_rev(follower):
    base, state = follower
    state.rollback = True
    status, _ = upload(base, b"payload", v="1234abc")
    assert status == 200
    wait_reboot(state)
    assert state.rev == "abc1234"  # came back on the OLD rev
