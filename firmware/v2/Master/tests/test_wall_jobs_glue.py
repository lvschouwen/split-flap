"""Source gates for the unit jobs and the row image offer of the wall link (#566).

The rules are natively tested (test_wall_jobs, test_wall_update_policy,
test_wall_link_core). What a native test cannot reach: that the job table
covers the link's every job code, that the master waits longer for a download
than the row lets one run, and that a row board's unit update holds the same
producers back as the master's own.
"""
import re
from pathlib import Path

MASTER = Path(__file__).resolve().parent.parent
V2 = MASTER.parent


def _code(path):
    return re.sub(r"//[^\n]*", "", Path(path).read_text())


def _define(code, name):
    return int(re.search(rf"#define {name} (\d+)", code).group(1))


def test_every_job_code_of_the_link_has_a_job_name():
    proto = (V2 / "link" / "wall_link.proto").read_text()
    enum = proto[proto.index("enum OpCode {"):]
    enum = enum[:enum.index("}")]
    codes = set(re.findall(r"\b(OPC_[A-Z_]+)\s*=", enum)) - {"OPC_NONE"}
    assert len(codes) >= 13
    table = _code(MASTER / "WallJobs.h")
    table = table[table.index("WALL_JOB_KINDS[]"):]
    table = table[:table.index("};")]
    named = re.findall(r'\{"([a-z-]+)",\s*wl_OpCode_(OPC_[A-Z_]+)', table)
    assert sorted(code for _, code in named) == sorted(codes)
    assert len({name for name, _ in named}) == len(named)


def test_every_event_code_of_a_row_has_a_name():
    proto = (V2 / "link" / "wall_link.proto").read_text()
    enum = proto[proto.index("enum RowEventCode {"):]
    enum = enum[:enum.index("}")]
    codes = set(re.findall(r"\b(ROW_EVT_[A-Z_]+)\s*=", enum)) - {"ROW_EVT_NONE"}
    assert len(codes) >= 3
    table = _code(MASTER / "WallRowEvents.h")
    table = table[table.index("WALL_ROW_EVENT_NAMES[]"):]
    table = table[:table.index("};")]
    named = re.findall(r'\{wl_RowEventCode_(ROW_EVT_[A-Z_]+),\s*"([a-z-]+)"', table)
    assert sorted(code for code, _ in named) == sorted(codes)
    assert len({name for _, name in named}) == len(named)


def test_the_api_index_names_every_job():
    index = (MASTER / "ApiIndex.h").read_text()
    line = next(l for l in index.splitlines() if '"/api/v2/action"' in l)
    table = _code(MASTER / "WallJobs.h")
    for name in re.findall(r'\{"([a-z-]+)",\s*wl_OpCode_', table):
        assert re.search(rf"\b{name}\b", line), name


def test_the_master_outwaits_the_rows_own_download_limit():
    row = _code(V2 / "FollowerEsp01" / "FollowerUpdatePolicy.h")
    master = _code(MASTER / "WallUpdatePolicy.h")
    # The timeout runs twice: for the connection, then for the answer.
    row_limit = (_define(row, "FOLLOWER_UPDATE_TOTAL_MS")
                 + 2 * _define(row, "FOLLOWER_UPDATE_HTTP_TIMEOUT_MS"))
    assert _define(master, "WALL_UPDATE_DOWNLOAD_MS") > row_limit


def test_a_row_boards_unit_update_holds_the_producers_back():
    # Every site that stands down for the master's own unit update, except
    # the ones that guard this board's own restart.
    sites = {
        "ClockTask.cpp": 1, "WebFirmware.cpp": 1, "MqttService.cpp": 1,
        "WebEndpoints.cpp": 4, "WebWall.cpp": 1,
    }
    for name, count in sites.items():
        code = _code(MASTER / name)
        gated = re.findall(
            r"reflashInProgress\([^;{]*?\)\s*(?:\|\||&&)\s*!?wallUnitUpdateRunning\(\)", code)
        assert len(gated) == count, (name, len(gated))
        assert code.count("reflashInProgress(") == count, name


def test_the_row_image_is_served_with_its_file_claimed():
    code = _code(MASTER / "WebFirmware.cpp")
    route = code[code.index('"/firmware/row", HTTP_GET'):]
    route = route[:route.index('"/firmware/master", HTTP_POST')]
    claim = route.index("followerImageTryClaimRelay()")
    release = route.index("followerImageReleaseRelay();", route.index("onDisconnect("))
    send = route.index("request->send(LittleFS, FOLLOWER_IMAGE_PATH")
    assert claim < release < send


def test_the_offer_rule_sees_one_clock():
    # Fed a later millis() from a hook, its ages against the pass's own "now"
    # go negative, which unsigned arithmetic reads as overdue.
    code = _code(MASTER / "WallLink.cpp")
    calls = re.findall(r"updater\.(?:hello|answer|tick|health|offered|nextCandidate)\([^;]*;", code)
    assert len(calls) >= 6
    for call in calls:
        assert "millis()" not in call, call
        assert "nowMs" in call or "passNowMs" in call, call


def test_forgetting_the_wifi_waits_for_what_a_restart_waits_for():
    code = _code(MASTER / "WebWall.cpp")
    body = code[code.index("void handleForgetWifi("):]
    body = body[:body.index("\n}\n")]
    assert body.index("webRestartRefusal()") < body.index("wifiStageReset()")
    restart = _code(MASTER / "WebEndpoints.cpp")
    restart = restart[restart.index("const char* webStageReboot("):]
    assert "webRestartRefusal()" in restart[:restart.index("\n}\n")]
