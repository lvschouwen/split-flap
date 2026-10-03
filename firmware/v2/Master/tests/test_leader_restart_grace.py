"""Source gate for #514: every place the leader restarts a member by pushing
an image must hand it to clusterMemberExpectRestart(), not re-roll the fields
by hand — the hand-rolled version is what let a rebooting ESP-01 row read as
STUCK + DEGRADED."""
import re
from pathlib import Path

MASTER = Path(__file__).resolve().parents[1]
SITES = {
    "ClusterLeaderRollout.cpp": 1,       # S3 image stream accepted
    "ClusterLeaderFollowerPush.cpp": 2,  # esp01 convergence + manual push
}


def test_every_push_completion_expects_the_restart():
    for name, want in SITES.items():
        src = (MASTER / name).read_text()
        assert len(re.findall(r"clusterMemberExpectRestart\(runtimes\[", src)) == want, name
        assert not re.search(r"runtimes\[\w+\]\.nextAttemptMs\s*=", src), (
            f"{name} schedules a member contact by hand; use the policy helper")
        assert not re.search(r"runtimes\[\w+\]\.joined\s*=\s*false", src), name


def test_each_expectation_follows_an_accepted_upload():
    for name in SITES:
        src = (MASTER / name).read_text()
        for m in re.finditer(r"clusterMemberExpectRestart\(", src):
            before = src[max(0, m.start() - 900):m.start()]
            assert "status == 200" in before, f"{name}: expectation not tied to a 200"
