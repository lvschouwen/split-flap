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


def test_the_wall_link_reports_completed_exchanges():
    # Every message read from a paired row board.
    link = _src("Master/WallLink.cpp")
    hook = link[link.index("void rowMessage(int row, const wl_ToMaster& message) override {"):]
    assert "wifiNoteConfirmedTraffic();" in hook[:hook.index("\n  }\n")]


def test_esp01_ladder_is_fed_the_confirmation():
    wifi = _src("FollowerEsp01/FollowerWifi.cpp")
    tick = wifi[wifi.index("void followerTxTick()"):]
    assert tick.index("in.trafficConfirmed = clusterLeaderContactFresh();") < tick.index(
        "wifiTxPolicyStep(")
    cluster = _src("FollowerEsp01/FollowerCluster.cpp")
    assert "return clusterFollowerContactFresh(policyState, millis());" in cluster
