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
    # #668: confirmed = the master acknowledged what the row wrote (its
    # uplink), not any message from the master (the downlink).
    wifi = _src("FollowerEsp01/FollowerWifi.cpp")
    tick = wifi[wifi.index("void followerTxTick()"):]
    assert tick.index("in.trafficConfirmed = linkUplinkConfirmed();") < tick.index(
        "wifiTxPolicyStep(")
    link = re.sub(r"//[^\n]*", "", _src("FollowerEsp01/FollowerLink.cpp"))
    assert "return followerLinkUplinkConfirmed(uplinkAck, millis());" in link
    write = link[link.index("bool writeWire(size_t n) {"):]
    write = write[:write.index("\n}\n")]
    assert write.index("sock.write(wire, n) == n") < write.index(
        "followerLinkNoteWrite(uplinkAck, (size_t)sock.availableForWrite(), TCP_SND_BUF, millis());") \
        < write.index("drop(")
    tick = link[link.index("void linkLoopTick() {"):]
    tick = tick[:tick.index("\n}\n")]
    assert "followerLinkNoteSendBuffer(uplinkAck, (size_t)sock.availableForWrite(), TCP_SND_BUF," \
        in tick
    assert "msgHeardMs, measurable);" in tick
    # Judged before the heard time is moved on by a busy or late pass.
    assert tick.index("followerLinkPassMeasurable(lastPassMs, passMs, unitOpsBusy())") \
        < tick.index("followerLinkHeardAfterPass(") < tick.index("lastPassMs = passMs;")
    assert tick.index("followerLinkNoteSendBuffer(") < tick.index("readFromMaster(view)")
    handle = link[link.index("void handle(const wl_ToRow& m, const FollowerClusterView& view) {"):]
    assert "msgHeardMs = lastHeardMs;" in handle[:handle.index("if (!welcomed)")]
    assert link.count("\n  msgHeardMs =") == 1  # only handle() sets it
    assert "followerLinkNoteOpened(uplinkAck, TCP_SND_BUF);" in link


def test_esp01_uplink_failures_feed_the_ladder_as_link_drops():
    link = re.sub(r"//[^\n]*", "", _src("FollowerEsp01/FollowerLink.cpp"))
    drop = link[link.index("void drop(const __FlashStringHelper* why, FollowerLinkClose kind"):]
    drop = drop[:drop.index("\n}\n")]
    # Judged before the socket is closed: the unacknowledged bytes go with it.
    assert drop.index("unackedBytes()") < drop.index("followerLinkUplinkFailed(kind, unacked, uplinkAck)") \
        < drop.index("followerTxUplinkFailed(why);") < drop.index("followerLinkNoteClosed(uplinkAck);") \
        < drop.index("sock.stop();")
    assert "followerLinkNoteClosed(uplinkAck);" in drop
    for close in ('drop(F("no Welcome"), FollowerLinkClose::NoWelcome);',
                  'drop(F("connection lost"), FollowerLinkClose::Lost);',
                  'drop(F("write failed"), FollowerLinkClose::WriteFailed);'):
        assert close in link, close
    assert link.count("FollowerLinkClose::NoWelcome)") == 1
    assert link.count("FollowerLinkClose::Lost)") == 1
    assert link.count("FollowerLinkClose::WriteFailed)") == 1
    wifi = re.sub(r"//[^\n]*", "", _src("FollowerEsp01/FollowerWifi.cpp"))
    owe = wifi[wifi.index("void followerTxUplinkFailed(const __FlashStringHelper* why) {"):]
    assert "followerLinkOweTxStep(txState)" in owe[:owe.index("\n}\n")]
