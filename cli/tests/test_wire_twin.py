"""Client parsing pinned against the repo's fake_follower wire twin (#278) —
the SAME server the firmware's leader-side pytest suite
(firmware/v2/Master/tests/test_fake_follower.py) drives, so
splitflap_client's parsing can't silently drift from the real /cluster wire.

Deviations from the naive "spawn fake_follower.py as a subprocess" approach,
found by reading its argparse block and its own pytest suite first:

* There is no `--port` flag — only `--count`/`--base-port` (CLI-bench use,
  per the module docstring: "python tests/fake_follower.py --count 4
  --base-port 8801" to hand rows to a real leader). `--plat` takes a bare
  tag, not a restricted choice.
* Its `do_GET` only answers `/cluster/digest` and `/cluster/health` — there
  is no `/settings` or `/units/health` here (those are S3/esp01-MASTER
  surfaces; this twin only impersonates a follower ROW). So both tests below
  exercise `ClusterHealth` against `/cluster/health`, the one model/endpoint
  pair the twin actually serves, instead of the brief's guessed `Settings`
  vs `/settings` pairing.
* The twin's OWN pytest suite (test_fake_follower.py's `follower()` fixture)
  and its leader-side counterpart (FollowerEsp01/tests/fake_leader.py, run
  by FollowerEsp01/tests/test_fake_leader.py) both drive it by importing
  `make_server()` and running it in-process on an OS-assigned port
  (port=0 -> server.server_address[1]), never via subprocess/CLI. That is
  the twin's documented "normal launch" for a pytest caller, so this file
  follows it instead of shelling out to a binary whose argparse the brief
  had to guess at.
"""
import sys
import threading
from pathlib import Path

import pytest

from splitflap_client.models import ClusterHealth
from splitflap_client.transport import BoardClient

TWIN_DIR = (Path(__file__).resolve().parents[2]
            / "firmware/v2/Master/tests")
TWIN_FILE = TWIN_DIR / "fake_follower.py"


@pytest.fixture()
def twin_url():
    if not TWIN_FILE.exists():
        pytest.skip("firmware/v2/Master/tests/fake_follower.py not present")

    sys.path.insert(0, str(TWIN_DIR))
    try:
        from fake_follower import make_server
    except ImportError as exc:
        pytest.skip(f"could not import fake_follower.py: {exc}")
    finally:
        sys.path.remove(str(TWIN_DIR))

    # port=0 -> OS-assigned free port, same as the twin's own fixture.
    server, _state = make_server(0, name="cli-wire-twin", rev="deadbeef",
                                 width=5, plat="esp01")
    port = server.server_address[1]
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield f"http://127.0.0.1:{port}"
    finally:
        server.shutdown()
        thread.join(timeout=5)


def test_cluster_health_standalone_before_join(twin_url):
    """Fresh twin, never joined: pins the client's parse of the
    "standalone" shape (state/leaderHost/rev; foreign+stackFree absent)."""
    with BoardClient(twin_url) as client:
        health = ClusterHealth.from_json(client.get_json("/cluster/health"))
    assert health.state == "standalone"
    assert health.leader_host == ""
    assert health.rev == "deadbeef"
    assert health.stack_free is None


def test_cluster_health_reflects_join(twin_url):
    """POST /cluster/join through the client, then re-GET /cluster/health —
    pins that the client's parse reflects a REAL state transition on the
    wire twin, not just a hand-fed fixture."""
    with BoardClient(twin_url) as client:
        client.post("/cluster/join", data={
            "leaderHost": "10.0.0.5", "row": "2", "epoch": "1",
            "leaderName": "row1-leader",
        })
        health = ClusterHealth.from_json(client.get_json("/cluster/health"))
    assert health.state == "clustered"
    assert health.leader_host == "10.0.0.5"
    assert health.leader_name == "row1-leader"
    assert health.row == 2
