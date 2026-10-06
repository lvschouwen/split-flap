"""Source gate for quiet mode's wiring (#227).

QuietPolicy.h decides what passes (natively tested). What a native test cannot
reach is that every producer of display content actually asks: the clock
ticker, the MQTT text path, the web drain, the link task telling the rows, and
the row board's fallback clock. One of them missing is a wall that flaps at
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
    own = code.index("if (tasksQuiet()) continue;")
    assert own < code.index("wallShowActive()"), "the wall reroute must come after the quiet gate"
    assert own < code.index("decideClockTick(")


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


def test_a_change_is_written_back_to_the_retained_command():
    code = _code(MASTER / "MqttService.cpp")
    body = code[code.index("static void mqttPublishQuiet()"):]
    body = body[:body.index("\n}\n")]
    mirror = body.index("mqttClient.publish(mqttQuietCmdTopic.c_str(), 0, true,")
    # Never on the first publish of a session: HA's retained command has not
    # arrived yet and must win.
    assert "if (mqttLastQuietState >= 0) {" in body[:mirror]
    assert body.index("if (mqttLastQuietState >= 0) {") < mirror < body.index("mqttLastQuietState = state;")


def test_the_row_board_reads_the_flag_and_holds_its_frame():
    # The ESP-01 row takes the flag from its master over the wall link, and
    # only on a connection that reached Welcome.
    esp = _code(V2 / "FollowerEsp01" / "FollowerLink.cpp")
    handle = esp[esp.index("void handle(const wl_ToRow& m"):]
    assert handle.index("if (!welcomed) {") < handle.index("clusterNoteLeaderQuiet(m.body.quiet.on);")
    fallback = _code(V2 / "FollowerEsp01" / "FollowerCluster.cpp")
    blank = fallback.index("if (followerPhaseShowsBlank(policyState.phase)) {")
    assert fallback.index("if (leaderQuiet) return;", blank) < fallback.index("followerClockEligible(", blank)
    assert "if (clockShowing && !leaderQuiet && !renderPending" in fallback


def test_the_web_drain_hands_text_to_the_wall_only_after_the_quiet_drop():
    code = _code(MASTER / "WebEndpoints.cpp")
    assert code.index("quietBlocksContent(settings.quiet, false)") < code.index("wallShowText(")


def test_the_link_task_tells_the_rows_the_quiet_state_every_pass():
    code = _code(MASTER / "WallLink.cpp")
    assert "core->setQuiet(tasksQuiet());" in code
