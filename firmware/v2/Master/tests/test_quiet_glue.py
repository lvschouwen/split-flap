"""Source gate for quiet mode's wiring (#227).

QuietPolicy.h decides what passes (natively tested). What a native test cannot
reach is that every producer of display content actually asks: the clock
ticker, the MQTT text path, the web drain, the leader's ping, and both member
implementations' fallback clocks. One of them missing is a wall that flaps at
night.
"""
import re
from pathlib import Path

MASTER = Path(__file__).resolve().parent.parent
V2 = MASTER.parent


def _code(path):
    return re.sub(r"//[^\n]*", "", path.read_text())


def test_clock_ticker_stands_down_before_any_content_path():
    code = _code(MASTER / "ClockTask.cpp")
    own = code.index("if (!membership.gated && tasksQuiet()) continue;")
    assert own < code.index("clusterLeaderEnabled()"), "the leader reroute must come after the quiet gate"
    assert own < code.index("decideClockTick(")
    # A member of a quiet wall moves nothing itself: no own clock in LeaderLost,
    # no segment re-show.
    member = code.index("if (cluster.gated && cluster.quiet) continue;")
    assert member < code.index("decideClockTick(")


def test_mqtt_text_is_dropped_unless_forced():
    code = _code(MASTER / "MqttService.cpp")
    text = code[code.index("case MqttCommand::Text:"):]
    gate = text.index("quietBlocksContent(tasksQuiet(), quietTextForced(payload))")
    assert gate < text.index("notificationStart("), "the gate must sit before the notification is shown"
    assert "subscribe(mqttQuietCmdTopic" in code
    assert "webMqttApplyQuiet(" in code


def test_web_drain_drops_text_after_applying_a_quiet_toggle():
    code = _code(MASTER / "WebEndpoints.cpp")
    applied = code.index("applySettingsPost(pendingPost, settings, store);")
    drop = code.index("quietBlocksContent(settings.quiet, false)")
    assert applied < drop, "a quiet toggle riding the POST must be applied first"
    block = code[drop:drop + 400]
    assert "messageProvided = false;" in block and "transientProvided = false;" in block


def test_every_leader_ping_carries_the_quiet_flag_before_the_signature():
    for name in ("ClusterLeaderFanout.cpp", "ClusterLeaderMaintenance.cpp"):
        code = _code(MASTER / name)
        assert "const bool pingQuiet = tasksQuiet();" in code, name
        suffix = code.index("clusterQuietPingSuffix(pingQuiet)")
        # ts/mac close the body: the flag goes in before them, in the same
        # block that builds this ping.
        sign = code.index('"&ts="', suffix)
        assert sign - suffix < 900, f"{name}: the quiet flag is not part of the ping it should ride"
        assert "you=" in code[suffix - 400:suffix], f"{name}: the flag must follow the ping's own fields"
        # A keyed member gets a mac for the flag, over the same value and ts.
        mac = code.index("CLUSTER_PING_QUIET_MAC_PARAM", suffix)
        assert "clusterQuietMsg(" in code[mac:mac + 300], name
        assert "pingQuiet" in code[mac:mac + 300], f"{name}: flag and mac must use one read of the state"


def test_leader_does_not_restore_its_own_row_while_quiet():
    code = _code(MASTER / "ClusterLeaderGrid.cpp")
    body = code[code.index("void serviceSelfRow()"):]
    body = body[:body.index("\n}\n")]
    gate = body.index("if (tasksQuiet()) return;")
    # After the in-flight render (already committed to the members), before
    # the re-show.
    assert body.index("if (selfPending)") < gate < body.rindex("displayEnqueue(")


def test_a_promoted_member_keeps_the_wall_quiet():
    code = _code(MASTER / "ClusterFollower.cpp")
    impl = code[code.index("static ClusterPromoteVerdict clusterFollowerPromoteImpl(bool autoPath) {"):]
    read = impl.index("wasQuiet = memberQuiet;")
    leave = impl.index("followerLeaveLocked();")
    apply = impl.index("if (wasQuiet) webMqttApplyQuiet(true);")
    assert read < leave < apply, "read the flag before the leave clears it, apply after"


def test_a_change_is_written_back_to_the_retained_command():
    code = _code(MASTER / "MqttService.cpp")
    body = code[code.index("static void mqttPublishQuiet()"):]
    body = body[:body.index("\n}\n")]
    mirror = body.index("mqttClient.publish(mqttQuietCmdTopic.c_str(), 0, true,")
    # Never on the first publish of a session: HA's retained command has not
    # arrived yet and must win.
    assert "if (mqttLastQuietState >= 0) {" in body[:mirror]
    assert body.index("if (mqttLastQuietState >= 0) {") < mirror < body.index("mqttLastQuietState = state;")


def test_both_member_implementations_read_the_flag_and_hold_their_frame():
    s3 = _code(MASTER / "WebCluster.cpp")
    assert "clusterQuietAccept(" in s3 and "clusterFollowerMacMatches(" in s3
    assert "clusterQuietMsg(pingTs," in s3, "the mac must be checked against this ping's timestamp"
    esp = _code(V2 / "FollowerEsp01" / "FollowerWeb.cpp")
    assert "clusterQuietAccept(" in esp and "clusterMacMatches(" in esp
    assert "clusterQuietMsg(pingTs," in esp
    handled = esp.index("if (!clusterHandlePing())")
    noted = esp.index("clusterNoteLeaderQuiet(")
    assert handled < noted, "only an accepted ping may set the flag"
    # The refusal branch returns before the flag is noted.
    assert "return;" in esp[handled:noted]
    fallback = _code(V2 / "FollowerEsp01" / "FollowerCluster.cpp")
    blank = fallback.index("if (followerPhaseShowsBlank(policyState.phase)) {")
    assert fallback.index("if (leaderQuiet) return;", blank) < fallback.index("followerClockEligible(", blank)
    assert "if (clockShowing && !leaderQuiet && !renderPending" in fallback
