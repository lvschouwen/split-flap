"""The campaign scripts against a stand-in for the master's /api/v2.

restore-unit-offsets.sh and commission-units.sh restore calibration after an
erase, so what they must never do is report success they did not see. Each
test here runs the real script (bash + curl) against a small HTTP server that
answers like the master, with one thing made to go wrong.
"""
import json
import os
import subprocess
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent.parent
MASTER_ID = "wall-master"
ROW_ID = "wall-row"
FIELDS = ["address", "level", "reason", "a", "b", "state", "rev", "firmware",
          "bootloader", "supplyMv", "supplyMinMv", "shows", "turns", "offset"]


class Wall:
    """What the stand-in master knows, and the faults a test switches on."""

    def __init__(self):
        self.units = {
            MASTER_ID: {a: self.unit(60 + a) for a in (1, 2, 3)},
            ROW_ID: {a: self.unit(50 + a) for a in (1, 2)},
        }
        self.brake_off = False       # updateUnitsAtStart
        self.ops = {}
        self.next_op = 1
        self.actions = []            # every job body, in order
        self.lose_offset_writes = False
        self.update_plans_nothing = False
        self.self_test_windows = []  # popped per self-test; empty = 38
        self.refuse = None           # (status, error) for every job
        self.row_lag_reads = 0       # board reads of the row that still show old facts
        self.row_stale = None

    @staticmethod
    def unit(offset):
        return {"state": "running", "status": "current", "rev": "abc1234",
                "offset": offset, "turns": 12, "home": "homed", "homeFailures": 0}

    def board_units(self, board):
        if board == ROW_ID and self.row_lag_reads > 0 and self.row_stale is not None:
            self.row_lag_reads -= 1
            return self.row_stale
        return self.units[board]

    def start(self, body):
        self.actions.append(body)
        if self.refuse:
            return self.refuse[0], {"error": self.refuse[1]}
        board = body["target"]["row"] or MASTER_ID
        unit = self.units[board].get(body["target"].get("unit"))
        op = {"op": self.next_op, "name": body["name"], "state": "done", "result": "ok"}
        self.next_op += 1
        name, args = body["name"], body.get("args", {})
        if unit is None:
            op.update(state="failed", reason="no running unit at that address")
            del op["result"]
        elif name == "set-offset":
            if board == ROW_ID:
                self.row_stale = json.loads(json.dumps(self.units[board]))
                self.row_stale = {int(k): v for k, v in self.row_stale.items()}
            if not self.lose_offset_writes:
                unit["offset"] = args["offset"]
        elif name == "update-units":
            if not self.update_plans_nothing:
                unit["status"] = "current"
        elif name == "reset-odometer":
            unit["turns"] = 0
        elif name == "jog":
            unit["jogs"] = unit.get("jogs", 0) + 1
        elif name == "self-test":
            window = self.self_test_windows.pop(0) if self.self_test_windows else 38
            op["data"] = {"state": "ok", "hall_window": window, "steps_per_rev": 2050}
        self.ops[op["op"]] = op
        return 202, {"op": op["op"]}

    def get(self, path):
        if path == "/api/v2/wall":
            return {"master": {"id": MASTER_ID},
                    "rows": [{"id": "", "own": True}, {"id": ROW_ID, "own": False}]}
        if path == "/api/v2/firmware":
            return {"units": {"shouldBe": "abc1234"}}
        if path == "/api/v2/settings/wall":
            return {"updateUnitsAtStart": self.brake_off}
        parts = path.split("/")
        if path.startswith("/api/v2/board/") and parts[4] in self.units:
            rows = []
            for addr, u in sorted(self.board_units(parts[4]).items()):
                row = [None] * len(FIELDS)
                row[FIELDS.index("address")] = addr
                row[FIELDS.index("offset")] = u["offset"]
                rows.append(row)
            doc = {"id": parts[4], "units": {"fields": FIELDS, "rows": rows}}
            if parts[4] == MASTER_ID:
                doc["unitUpdate"] = {"total": 0 if self.update_plans_nothing else 1}
            return doc
        if path.startswith("/api/v2/unit/") and parts[4] in self.units:
            u = self.units[parts[4]].get(int(parts[5]))
            if u is None:
                return None
            return {"state": u["state"],
                    "firmware": {"rev": u["rev"], "status": u["status"]},
                    "drum": {"offset": u["offset"], "turns": u["turns"], "home": u["home"],
                             "homeFailures": u["homeFailures"]}}
        if path.startswith("/api/v2/op/"):
            return self.ops.get(int(parts[4]))
        return None


@pytest.fixture
def wall():
    state = Wall()

    class Handler(BaseHTTPRequestHandler):
        def answer(self, status, doc):
            raw = json.dumps(doc).encode()
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(raw)))
            self.end_headers()
            self.wfile.write(raw)

        def do_GET(self):
            doc = state.get(self.path)
            self.answer(200 if doc is not None else 404, doc if doc is not None else {})

        def do_POST(self):
            body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            assert self.path == "/api/v2/action"
            assert self.headers["Content-Type"] == "application/json"
            self.answer(*state.start(body))

        def log_message(self, *args):
            pass

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    state.address = f"127.0.0.1:{server.server_address[1]}"
    yield state
    server.shutdown()


@pytest.fixture
def capture(tmp_path, wall):
    path = tmp_path / "unit-offsets.json"
    path.write_text(json.dumps({
        "master": wall.address,
        "rows": [
            {"board": MASTER_ID, "offsets": {"1": 61, "2": 62, "3": 63}},
            {"board": ROW_ID, "offsets": {"1": 51, "2": 52}},
        ]}))
    return path


def run(script, capture, *args):
    env = dict(os.environ, UNIT_OFFSETS_JSON=str(capture), WALL_POLL_S="0.05",
               WALL_READBACK_S="2", COMMISSION_COOL_S="0", COMMISSION_FACT_S="1",
               COMMISSION_ROUNDS="1")
    return subprocess.run(["bash", str(HERE / script), *args], env=env,
                          capture_output=True, text=True, timeout=120)


def jobs(wall, name):
    return [a for a in wall.actions if a["name"] == name]


# --- restore-unit-offsets.sh --------------------------------------------------

def test_verify_passes_when_the_wall_matches_the_capture(wall, capture):
    done = run("restore-unit-offsets.sh", capture)
    assert done.returncode == 0, done.stdout + done.stderr
    assert done.stdout.count(" ok") == 5
    assert wall.actions == []


def test_verify_fails_on_a_unit_that_differs(wall, capture):
    wall.units[ROW_ID][2]["offset"] = 9
    done = run("restore-unit-offsets.sh", capture)
    assert done.returncode == 1
    assert "DIFFERS" in done.stdout


def test_verify_fails_on_a_unit_the_master_did_not_read(wall, capture):
    del wall.units[MASTER_ID][3]
    done = run("restore-unit-offsets.sh", capture)
    assert done.returncode == 1
    assert "UNREADABLE" in done.stdout


def test_a_board_that_is_not_in_the_capture_is_an_error(wall, capture):
    done = run("restore-unit-offsets.sh", capture, "--board", "wall-rwo")
    assert done.returncode == 1
    assert "no units matched" in done.stderr


def test_apply_writes_only_what_differs_and_names_the_own_row_empty(wall, capture):
    wall.units[MASTER_ID][2]["offset"] = 0
    wall.units[ROW_ID][1]["offset"] = 0
    done = run("restore-unit-offsets.sh", capture, "--apply")
    assert done.returncode == 0, done.stdout + done.stderr
    assert wall.actions == [
        {"name": "set-offset", "target": {"row": "", "unit": 2}, "args": {"offset": 62}},
        {"name": "set-offset", "target": {"row": ROW_ID, "unit": 1}, "args": {"offset": 51}},
    ]
    assert done.stdout.count("restored") == 2


def test_apply_waits_for_a_row_board_to_report_the_new_offset(wall, capture):
    wall.units[ROW_ID][1]["offset"] = 0
    wall.row_lag_reads = 4  # the first read is the compare; three stale read-backs follow
    done = run("restore-unit-offsets.sh", capture, "--apply", "--board", ROW_ID)
    assert done.returncode == 0, done.stdout + done.stderr
    assert "restored" in done.stdout


def test_apply_fails_when_a_write_is_acknowledged_but_did_not_land(wall, capture):
    wall.units[MASTER_ID][1]["offset"] = 0
    wall.lose_offset_writes = True
    done = run("restore-unit-offsets.sh", capture, "--apply")
    assert done.returncode == 1
    assert "MISMATCH" in done.stdout


def test_apply_reports_why_the_master_refused(wall, capture):
    wall.units[MASTER_ID][1]["offset"] = 0
    wall.refuse = (409, "a unit update is running, retry when it has finished")
    done = run("restore-unit-offsets.sh", capture, "--apply")
    assert done.returncode == 1
    assert "WRITE FAILED: refused (409): a unit update is running" in done.stdout


def test_capture_takes_the_walls_values_and_keeps_what_was_not_read(wall, capture):
    wall.units[MASTER_ID][1]["offset"] = 77
    del wall.units[ROW_ID][2]
    done = run("restore-unit-offsets.sh", capture, "--capture")
    assert done.returncode == 0, done.stdout + done.stderr
    rows = {r["board"]: r["offsets"] for r in json.loads(capture.read_text())["rows"]}
    assert rows[MASTER_ID] == {"1": 77, "2": 62, "3": 63}
    assert rows[ROW_ID] == {"1": 51, "2": 52}
    assert "NOT READ" in done.stdout


def test_a_master_that_does_not_answer_is_an_error(wall, capture):
    done = run("restore-unit-offsets.sh", capture, "--master", "127.0.0.1:1")
    assert done.returncode == 1
    assert "no /api/v2 answer" in done.stderr


# --- commission-units.sh ------------------------------------------------------

def match_capture(wall):
    for board, base in ((MASTER_ID, 60), (ROW_ID, 50)):
        for addr, unit in wall.units[board].items():
            unit["offset"] = base + addr


def test_commission_runs_every_step_on_a_row_boards_unit(wall, capture):
    match_capture(wall)
    wall.units[ROW_ID][2].update(status="outdated", offset=0)
    done = run("commission-units.sh", capture, "--board", ROW_ID, "--only", "2")
    assert done.returncode == 0, done.stdout + done.stderr
    names = [a["name"] for a in wall.actions]
    assert names[:4] == ["update-units", "set-offset", "reset-odometer", "home"]
    assert names[4:] == ["jog"] * 17 + ["self-test"] + ["jog"] * 17 + ["self-test"]
    assert all(a["target"] == {"row": ROW_ID, "unit": 2} for a in wall.actions)
    assert wall.actions[0]["args"] == {"force": 1}
    assert wall.units[ROW_ID][2]["offset"] == 52
    assert "PASS        a2 commissioned" in done.stdout


def test_commission_skips_the_update_of_a_current_unit(wall, capture):
    match_capture(wall)
    done = run("commission-units.sh", capture, "--board", MASTER_ID, "--only", "1")
    assert done.returncode == 0, done.stdout + done.stderr
    assert jobs(wall, "update-units") == []


def test_commission_refuses_while_boards_update_units_at_start(wall, capture):
    wall.brake_off = True
    done = run("commission-units.sh", capture, "--board", MASTER_ID)
    assert done.returncode == 1
    assert "REFUSING" in done.stderr
    assert wall.actions == []


def test_commission_refuses_a_board_the_wall_does_not_have(wall, capture):
    data = json.loads(capture.read_text())
    data["rows"].append({"board": "gone-row", "offsets": {"1": 5}})
    capture.write_text(json.dumps(data))
    done = run("commission-units.sh", capture, "--board", "gone-row")
    assert done.returncode == 1
    assert "not a board of the wall" in done.stderr
    assert wall.actions == []


def test_commission_stops_when_the_update_planned_nothing(wall, capture):
    match_capture(wall)
    wall.units[MASTER_ID][1]["status"] = "outdated"
    wall.update_plans_nothing = True
    done = run("commission-units.sh", capture, "--board", MASTER_ID)
    assert done.returncode == 1
    assert "nothing was planned for a1" in done.stdout
    assert "STOPPED at a1" in done.stdout
    assert [a["name"] for a in wall.actions] == ["update-units"]


def test_commission_stops_when_a_row_units_update_changed_nothing(wall, capture):
    match_capture(wall)
    wall.units[ROW_ID][1]["status"] = "outdated"
    wall.update_plans_nothing = True
    done = run("commission-units.sh", capture, "--board", ROW_ID)
    assert done.returncode == 1
    assert "firmware reads 'outdated', wanted current" in done.stdout
    assert [a["name"] for a in wall.actions] == ["update-units"]


def test_commission_stops_at_an_offset_that_did_not_land(wall, capture):
    wall.units[MASTER_ID][1]["offset"] = 0
    wall.lose_offset_writes = True
    done = run("commission-units.sh", capture, "--board", MASTER_ID)
    assert done.returncode == 1
    assert "offset read back '0', wanted '61'" in done.stdout
    assert jobs(wall, "home") == []
    assert all(a["target"]["unit"] == 1 for a in wall.actions)


def test_commission_stops_when_the_self_test_drifts(wall, capture):
    match_capture(wall)
    wall.self_test_windows = [38, 50]
    done = run("commission-units.sh", capture, "--board", MASTER_ID)
    assert done.returncode == 1
    assert "self-test drifted across the run: hall_window 38 -> 50" in done.stdout
    assert all(a["target"]["unit"] == 1 for a in wall.actions)


def test_commission_stops_at_a_unit_that_is_not_there(wall, capture):
    match_capture(wall)
    del wall.units[MASTER_ID][2]
    done = run("commission-units.sh", capture, "--board", MASTER_ID, "--from", "2")
    assert done.returncode == 1
    assert "STOPPED at a2" in done.stdout
    assert all(a["target"]["unit"] == 2 for a in wall.actions)
