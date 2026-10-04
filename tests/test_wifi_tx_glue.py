"""Source gate for #515: the TX ladder only stops climbing on a weak signal
estimate if the glue actually feeds it "traffic confirmed". The rule is pure
and unit-tested; these are the call sites no native test reaches."""
import re
from pathlib import Path

V2 = Path(__file__).resolve().parents[1] / "firmware" / "v2"


def _src(rel):
    return (V2 / rel).read_text()


def test_s3_ladder_is_fed_the_confirmation():
    wifi = _src("Master/WifiService.cpp")
    assert re.search(r"in\.trafficConfirmed\s*=\s*wifiTxTrafficFresh\(", wifi)
    step = wifi[wifi.index("static bool txPowerStep("):]
    assert step.index("in.trafficConfirmed") < step.index("wifiTxPolicyStep(")


def test_s3_cluster_paths_report_completed_exchanges():
    # Leader side: both places a member contact is recorded as answered.
    fanout = _src("Master/ClusterLeaderFanout.cpp")
    assert len(re.findall(
        r"clusterMemberOnSuccess\(m, nowMs\);\s*wifiNoteConfirmedTraffic\(\);", fanout)) == 2
    # Follower side: a served join and a served ping.
    follower = _src("Master/ClusterFollower.cpp")
    assert follower.count("wifiNoteConfirmedTraffic();") == 2
    assert re.search(
        r"if \(!clusterFollowerContact\(policyState, millis\(\)\)\) return false;\s*"
        r"wifiNoteConfirmedTraffic\(\);", follower)


def test_esp01_ladder_is_fed_the_confirmation():
    wifi = _src("FollowerEsp01/FollowerWifi.cpp")
    tick = wifi[wifi.index("void followerTxTick()"):]
    assert tick.index("in.trafficConfirmed = clusterLeaderContactFresh();") < tick.index(
        "wifiTxPolicyStep(")
    cluster = _src("FollowerEsp01/FollowerCluster.cpp")
    assert "return clusterFollowerContactFresh(policyState, millis());" in cluster
