"""Pins the ESP-01 follower's wire contract from the LEADER side (#298):
the fake leader (this project's bench driver) runs against the python twin
of the follower (the v2 master's fake_follower.py, plat=esp01 variant) so
the two harnesses can never drift from the shared /cluster + /firmware
shapes. The real firmware paths are bench tier (spec drill list)."""

import json
import pathlib
import sys
import threading

import pytest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
# The follower twin lives with the v2 master's tests — one twin, two users.
sys.path.insert(
    0, str(pathlib.Path(__file__).resolve().parents[2] / "Master" / "tests"))

from fake_follower import make_server  # noqa: E402
from fake_leader import FakeLeader, stream_firmware  # noqa: E402


@pytest.fixture()
def esp01():
    server, state = make_server(0, name="esp01-row", rev="abc1234", width=8,
                                plat="esp01", reboot_secs=0.1)
    port = server.server_address[1]
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    yield f"http://127.0.0.1:{port}", state
    server.shutdown()


@pytest.fixture()
def leader():
    return FakeLeader(leader_host="192.168.15.22", leader_name="wall-leader",
                      epoch=7)


def test_join_handshake_carries_esp01_identity(esp01, leader):
    base, _ = esp01
    status, body = leader.join(base, row=1)
    assert status == 200
    reply = json.loads(body)
    assert reply["name"] == "esp01-row"
    assert reply["rev"] == "abc1234"
    assert reply["width"] == 8
    assert reply["plat"] == "esp01"
    for key in ("detected", "faulty", "faultMask", "wear",
                "heap", "rssi", "up"):
        assert key in reply


def test_render_applies_verbatim_with_commit_at(esp01, leader):
    base, state = esp01
    leader.join(base, row=1)
    status, body = leader.render(base, "HELLO ROW", commit_lead_ms=400)
    assert status == 200
    assert json.loads(body) == {"applied": True, "seq": 1}
    assert state.segment == "HELLO ROW"
    assert state.renders[0]["commitAtMs"] > 0


def test_duplicate_seq_is_not_reapplied(esp01, leader):
    base, state = esp01
    leader.join(base)
    leader.render(base, "FIRST", seq=5)
    status, body = leader.render(base, "STALE RETRY", seq=5)
    assert json.loads(body)["applied"] is False
    assert state.segment == "FIRST"


def test_leader_reboot_new_epoch_restarts_seq_space(esp01, leader):
    base, state = esp01
    leader.join(base)
    leader.render(base, "OLD", seq=50)
    leader.epoch = 9  # leader rebooted
    leader.join(base)
    status, body = leader.render(base, "NEW EPOCH", seq=1)
    assert json.loads(body)["applied"] is True
    assert state.segment == "NEW EPOCH"


def test_render_before_join_tells_leader_to_rejoin(esp01, leader):
    base, _ = esp01
    status, body = leader.render(base, "NO MEMBERSHIP")
    assert status == 409
    assert json.loads(body) == {"error": "not clustered"}


def test_ping_reply_carries_health_plat_and_vitals(esp01, leader):
    base, _ = esp01
    leader.join(base)
    status, body = leader.ping(base)
    assert status == 200
    reply = json.loads(body)
    assert reply["state"] == "clustered"
    assert reply["plat"] == "esp01"
    for key in ("width", "detected", "faulty", "faultMask", "wear", "rev",
                "heap", "rssi", "up"):
        assert key in reply


def test_digest_piggyback_is_harmless_to_the_dumb_row(esp01, leader):
    # The leader piggybacks the digest on every ping (#294); the ESP-01
    # ignores it by design (never a takeover candidate). The ping must
    # still count as contact.
    base, _ = esp01
    leader.join(base)
    status, _ = leader.ping(base, digest='{"gen":1,"table":"x|0|0|8"}', you=1)
    assert status == 200


def test_leave_returns_the_row_to_standalone(esp01, leader):
    base, _ = esp01
    leader.join(base)
    status, _ = leader.leave(base)
    assert status == 200
    status, _ = leader.ping(base)
    assert status == 409


def test_foreign_join_while_leader_fresh_is_409_other_leader(esp01, leader):
    base, _ = esp01
    leader.join(base)
    usurper = FakeLeader(leader_host="10.9.9.9", leader_name="usurper",
                         epoch=99)
    status, body = usurper.join(base)
    assert status == 409
    reply = json.loads(body)
    assert reply["error"] == "other-leader"
    assert reply["leaderHost"] == "192.168.15.22"


def test_firmware_stream_md5_mismatch_is_rejected(esp01, leader):
    base, state = esp01
    status, body = stream_firmware(base, b"\xe9payload-bytes" * 50,
                                   md5="0" * 32)
    assert status == 500
    assert "MD5" in body
    assert state.rev == "abc1234"  # nothing flashed


def test_firmware_stream_good_md5_flashes(esp01, leader):
    # ota-flash.sh's path (the REAL leader never streams at esp01 — #297).
    base, state = esp01
    image = b"\xe9follower-image" * 64
    status, body = stream_firmware(base, image, v="1234abc")
    assert status == 200
    deadline = 50
    while state.rebooting and deadline:
        threading.Event().wait(0.02)
        deadline -= 1
    assert state.uploads[0]["size"] == len(image)
    assert state.rev == "1234abc"
