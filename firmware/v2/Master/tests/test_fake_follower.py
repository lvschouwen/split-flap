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
# of test_multipart_frame_matches_upload_contract. The boundary derives from
# the image md5 (#292): a fixed boundary constant is embedded in the leader's
# own image and truncates the transfer on the device parser.
LEGACY_FIXED_BOUNDARY = "splitflapClusterRollout"  # pre-#292 regression pin


def rollout_boundary(image):
    """Pytest twin of clusterRolloutBoundary() — an image cannot contain
    its own md5, so the derived delimiter provably never collides."""
    return "sfr-" + hashlib.md5(image).hexdigest()


def rollout_frame(image, boundary):
    preamble = (
        f"--{boundary}\r\n"
        'Content-Disposition: form-data; name="firmware"; '
        'filename="firmware.bin"\r\n'
        "Content-Type: application/octet-stream\r\n\r\n"
    ).encode()
    trailer = f"\r\n--{boundary}--\r\n".encode()
    return preamble + image + trailer


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


def get_allow_error(base, path):
    try:
        return get(base, path)
    except urllib.error.HTTPError as error:
        return error.code, error.read().decode()


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


def test_join_reply_carries_identity_rev_width_and_health(follower):
    base, _ = follower
    status, body = join(base)
    assert status == 200
    reply = json.loads(body)
    assert reply == {"name": "wall-2", "rev": "abc1234", "width": 16,
                     "detected": 16, "faulty": 0, "faultMask": "0000",
                     "wear": False, "protocol": 1}


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


def test_ping_after_join_reports_clustered_state_and_health(follower):
    base, _ = follower
    join(base, epoch=7)
    render(base, 7, 3, "X")
    status, body = post(base, "/cluster/ping")
    assert status == 200
    assert json.loads(body) == {"state": "clustered", "epoch": 7, "seq": 3,
                                "width": 16, "detected": 16, "faulty": 0,
                                "faultMask": "0000", "wear": False,
                                "rev": "abc1234"}


def test_fault_drill_shows_up_in_ping_health(follower):
    # POST /drill/fault flips the advertised fault bitmap so bench drills
    # can watch the leader's wall strips react without breaking hardware.
    base, _ = follower
    join(base)
    post(base, "/drill/fault", {"mask": "0005"})
    _, body = post(base, "/cluster/ping")
    reply = json.loads(body)
    assert reply["faultMask"] == "0005"
    assert reply["faulty"] == 2


# --- ping digest piggyback (#294 rung 2) --------------------------------------------

def test_digest_404_before_any_ping_carried_one(follower):
    base, _ = follower
    join(base)
    status, _ = get_allow_error(base, "/cluster/digest")
    assert status == 404


def test_ping_digest_is_stored_and_served(follower):
    base, state = follower
    join(base)
    digest = json.dumps({"gen": 3, "leader": {"name": "L", "host": "10.0.0.9"},
                         "table": "|0|0|16;192.168.15.91|1|0|16",
                         "rows": ["A", "B"], "status": {"enabled": True}})
    status, _ = post(base, "/cluster/ping", {"digest": digest, "you": 1})
    assert status == 200
    assert state.self_index == 1
    status, body = get(base, "/cluster/digest")
    assert status == 200
    reply = json.loads(body)
    assert reply["digest"]["gen"] == 3
    assert reply["digest"]["table"] == "|0|0|16;192.168.15.91|1|0|16"
    assert reply["ageMs"] >= 0


def test_leave_drops_the_stored_digest(follower):
    base, _ = follower
    join(base)
    post(base, "/cluster/ping", {"digest": "{\"gen\":1,\"table\":\"t\"}",
                                 "you": 0})
    post(base, "/cluster/leave")
    status, _ = get_allow_error(base, "/cluster/digest")
    assert status == 404


# --- sticky leadership (#295) --------------------------------------------------------

def test_foreign_join_while_leader_alive_is_409_other_leader(follower):
    base, _ = follower
    join(base)  # leaderHost 192.168.15.22, fresh contact
    status, body = post(base, "/cluster/join",
                        {"leaderName": "usurper", "leaderHost": "10.9.9.9",
                         "row": 0, "epoch": 99})
    assert status == 409
    reply = json.loads(body)
    assert reply["error"] == "other-leader"
    assert reply["leaderHost"] == "192.168.15.22"
    assert reply["leaderName"] == "wall-leader"


def test_same_leader_rejoin_never_conflicts(follower):
    base, _ = follower
    join(base, epoch=7)
    status, _ = join(base, epoch=8)
    assert status == 200


def test_foreign_join_after_leader_silence_is_accepted(follower):
    base, state = follower
    join(base)
    state.contact_fresh_secs = 0.05  # shrink the 25 s window for the test
    time.sleep(0.1)
    status, _ = post(base, "/cluster/join",
                     {"leaderName": "successor", "leaderHost": "10.9.9.9",
                      "row": 1, "epoch": 99})
    assert status == 200


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

def upload(base, image, md5=None, v="1234abc", query=None, boundary=None):
    """Stream `image` exactly the way the leader's rollout does."""
    if boundary is None:
        boundary = rollout_boundary(image)
    body = rollout_frame(image, boundary)
    if query is None:
        digest = md5 if md5 is not None else hashlib.md5(image).hexdigest()
        query = f"?md5={digest}&v={v}"
    request = urllib.request.Request(
        base + "/firmware/master" + query, data=body, method="POST",
        headers={"Content-Type":
                 f"multipart/form-data; boundary={boundary}"})
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


def test_upload_boundary_inside_payload_truncates_like_the_device(follower):
    # #292 regression: the leader's image contains any compile-time boundary
    # constant as a string literal, and the device parser ends the file field
    # at the FIRST in-body hit — full-length body, truncated payload, MD5
    # verdict fails. The strict parser above must reproduce that.
    base, state = follower
    image = (b"\xe9head-bytes" +
             f"\r\n--{LEGACY_FIXED_BOUNDARY}--\r\n".encode() +
             b"tail-bytes" * 8)
    status, body = upload(base, image, boundary=LEGACY_FIXED_BOUNDARY)
    assert status == 500
    assert "MD5" in body
    assert state.rev == "abc1234"  # nothing flashed


def test_upload_md5_derived_boundary_survives_boundary_like_payload(follower):
    # Same payload, boundary derived from the image md5 (the #292 fix):
    # the delimiter provably cannot occur inside the image, transfer lands.
    base, state = follower
    image = (b"\xe9head-bytes" +
             f"\r\n--{LEGACY_FIXED_BOUNDARY}--\r\n".encode() +
             b"tail-bytes" * 8)
    status, body = upload(base, image)
    assert status == 200
    wait_reboot(state)
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


# --- ESP-01 platform variant (#297) --------------------------------------------------

@pytest.fixture()
def esp01_follower():
    """The #298 dumb-row follower's wire shape: plat=esp01 + vitals on the
    join/ping replies — the leader must parse plat and NEVER stream its S3
    image at this member (rollout exclusion pinned natively in
    test_cluster_rollout_policy; this fixture is the bench drill vehicle)."""
    server, state = make_server(0, name="esp01-row", rev="abc1234", width=8,
                                plat="esp01", reboot_secs=0.1)
    port = server.server_address[1]
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    yield f"http://127.0.0.1:{port}", state
    server.shutdown()


def test_esp01_join_reply_carries_plat_and_vitals(esp01_follower):
    base, _ = esp01_follower
    status, body = join(base)
    assert status == 200
    reply = json.loads(body)
    assert reply["plat"] == "esp01"
    for key in ("heap", "rssi", "up"):
        assert isinstance(reply[key], int)


def test_esp01_ping_reply_carries_plat_and_vitals(esp01_follower):
    base, _ = esp01_follower
    join(base)
    status, body = post(base, "/cluster/ping")
    assert status == 200
    reply = json.loads(body)
    assert reply["plat"] == "esp01"
    assert reply["state"] == "clustered"
    for key in ("heap", "rssi", "up"):
        assert isinstance(reply[key], int)


def test_default_follower_reports_no_plat(follower):
    # Absent plat = same platform as the leader — the existing exact-shape
    # join test pins it too; this makes the #297 contract explicit.
    base, _ = follower
    _, body = join(base)
    assert "plat" not in json.loads(body)


# --- deviceRole variant (#332) --------------------------------------------------------

@pytest.fixture()
def monitor_follower():
    """An S3 follower reporting deviceRole=headless-monitor: the additive
    role key on join/ping replies feeds the leader's succession tiers
    (ordering pinned natively in test_cluster_digest; this fixture is the
    wire-shape pin + bench drill vehicle)."""
    server, state = make_server(0, name="monitor-node", rev="abc1234",
                                width=0, role="headless-monitor",
                                reboot_secs=0.1)
    port = server.server_address[1]
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    yield f"http://127.0.0.1:{port}", state
    server.shutdown()


def test_role_join_reply_carries_role(monitor_follower):
    base, _ = monitor_follower
    status, body = join(base)
    assert status == 200
    assert json.loads(body)["role"] == "headless-monitor"


def test_role_ping_reply_carries_role(monitor_follower):
    base, _ = monitor_follower
    join(base)
    status, body = post(base, "/cluster/ping")
    assert status == 200
    reply = json.loads(body)
    assert reply["role"] == "headless-monitor"
    assert reply["state"] == "clustered"


def test_default_follower_reports_no_role(follower):
    # Absent role = pre-#332 peer — the leader's tiers keep the old
    # width-0-preferred rule for it.
    base, _ = follower
    _, body = join(base)
    assert "role" not in json.loads(body)


def test_rollback_drill_keeps_old_rev(follower):
    base, state = follower
    state.rollback = True
    status, _ = upload(base, b"payload", v="1234abc")
    assert status == 200
    wait_reboot(state)
    assert state.rev == "abc1234"  # came back on the OLD rev
