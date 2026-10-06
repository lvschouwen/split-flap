"""The two bench stand-ins speak to each other.

fake_master.py is what the row firmware was proven against, fake_row.py is
what the master firmware is tried against. Run against each other, neither can
drift from the link the other was proven on.
"""
import json
import os
import socket
import subprocess
import sys
import time
from pathlib import Path

import pytest

pytest.importorskip("grpc_tools", reason="needs grpcio-tools to generate the Python protobuf module")

LINK = Path(__file__).resolve().parents[1]


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class Pair:
    def __init__(self, tmp: Path, row_args=()) -> None:
        subprocess.run([sys.executable, "-m", "grpc_tools.protoc", f"--proto_path={LINK}",
                        f"--python_out={tmp}", "wall_link.proto"], check=True)
        self.image = tmp / "follower-newrev01-gz.bin"
        self.image.write_bytes(b"\x1f\x8b" + bytes(range(256)) * 40)
        self.master_cmds, self.row_cmds = tmp / "master.cmd", tmp / "row.cmd"
        self.master_cmds.touch()
        self.row_cmds.touch()
        self.logs = {"master": tmp / "master.log", "row": tmp / "row.log"}
        port, http = free_port(), free_port()
        env = {**os.environ, "PYTHONPATH": f"{tmp}{os.pathsep}{LINK}", "PYTHONUNBUFFERED": "1"}
        self.procs = [
            subprocess.Popen([sys.executable, str(LINK / "fake_master.py"), "--id", "bench-master",
                              "--port", str(port), "--commands", str(self.master_cmds),
                              "--image", str(self.image), "--http-port", str(http)],
                             stdout=self.logs["master"].open("w"), stderr=subprocess.STDOUT, env=env),
        ]
        self.wait("master", lambda e: e["kind"] == "listening")
        self.procs.append(
            subprocess.Popen([sys.executable, str(LINK / "fake_row.py"), "--master", "127.0.0.1",
                              "--port", str(port), "--id", "row-bench", "--width", "5",
                              "--commands", str(self.row_cmds), "--render-s", "0.1",
                              "--job-scale", "0.02", "--backoff-scale", "0.1", *row_args],
                             stdout=self.logs["row"].open("w"), stderr=subprocess.STDOUT, env=env))

    def entries(self, who: str) -> list[dict]:
        out = []
        for line in self.logs[who].read_text().splitlines():
            try:
                out.append(json.loads(line))
            except ValueError:
                raise AssertionError(f"{who} printed: {line}")
        return out

    def wait(self, who: str, match, timeout: float = 10) -> dict:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            for entry in self.entries(who):
                if match(entry):
                    return entry
            time.sleep(0.05)
        raise AssertionError(f"not seen in the {who} log:\n{self.logs[who].read_text()}")

    def master(self, line: str) -> None:
        with self.master_cmds.open("a") as handle:
            handle.write(line + "\n")

    def row(self, line: str) -> None:
        with self.row_cmds.open("a") as handle:
            handle.write(line + "\n")

    def close(self) -> None:
        for proc in self.procs:
            proc.terminate()
        for proc in self.procs:
            proc.wait(5)


@pytest.fixture
def pair(tmp_path):
    p = Pair(tmp_path)
    try:
        p.wait("master", lambda e: e.get("type") == "units")
        yield p
    finally:
        p.close()


def got(kind: str, **fields):
    return lambda e: e["kind"] == "got" and e.get("type") == kind and all(
        e.get(k) == v for k, v in fields.items())


def test_hello_welcome_status_and_unit_facts(pair):
    hello = pair.wait("master", got("hello"))["hello"]
    assert hello["id"] == "row-bench" and hello["width"] == 5 and hello["protocol"] == 1
    assert pair.wait("row", got("welcome"))["master_id"] == "bench-master"
    assert pair.wait("master", got("status"))["status"]["heap"] == 33000
    units = pair.wait("master", got("units"))["units"]
    assert units["width"] == 5 and [u["addr"] for u in units["units"]] == [1, 2, 3, 4, 5]


def test_show_is_answered_with_shown_and_ping_with_pong(pair):
    pair.master("show hello")
    assert pair.wait("row", got("show"))["text"] == "HELLO"
    assert pair.wait("master", got("shown"))["shown"]["render_id"] == 1
    pair.master("ping")
    pair.wait("master", got("pong"))


def test_a_unit_job_runs_busy_and_returns_its_result(pair):
    pair.master("op self_test 2")
    done = pair.wait("master", lambda e: got("op_state")(e) and e.get("phase") == "OP_OK")
    assert done["op_id"] == 1 and done["result"] == {"ok": True, "steps": 2038}
    statuses = [e["status"].get("busy", False) for e in pair.entries("master") if got("status")(e)]
    assert True in statuses and statuses[-1] is False


def test_a_boot_dump_arrives_whole_from_its_pieces(pair):
    pair.master("op boot_dump 1")
    done = pair.wait("master", lambda e: got("op_state")(e) and e.get("phase") == "OP_OK")
    assert done["result_bytes"] == 1024


def test_a_job_for_a_unit_the_row_does_not_have_is_refused(pair):
    pair.master("op home 9")
    refused = pair.wait("master", got("op_state", phase="OP_REFUSED"))
    assert refused["reason"] == 5  # REFUSAL_NO_UNIT


def test_an_offered_image_is_downloaded_and_the_row_returns_on_it(pair):
    first_boot = pair.wait("master", got("hello"))["hello"]["boot_id"]
    pair.master("update")
    pair.wait("master", lambda e: e["kind"] == "http" and e["bytes"] == pair.image.stat().st_size)
    back = pair.wait("master", lambda e: got("hello")(e) and e["hello"]["rev"] == "newrev01")
    assert back["hello"]["boot_id"] != first_boot
    assert pair.wait("master", got("update_state"))["update_state"]["rev"] == "newrev01"
    installed = [e for e in pair.entries("master")
                 if got("update_state")(e) and e["update_state"].get("phase") == "UPDATE_INSTALLED"]
    assert installed


def test_a_damaged_offer_leaves_the_row_on_its_rev(pair):
    pair.master("update-bad md5")
    failed = pair.wait("master", lambda e: got("update_state")(e)
                       and e["update_state"].get("phase") == "UPDATE_FAILED")
    assert failed["update_state"]["reason"] == "UPDATE_FLASH"
    pair.master("ping")
    pair.wait("master", got("pong"))
    assert all(e["hello"]["rev"] == "fakerow1" for e in pair.entries("master") if got("hello")(e))


def test_a_dropped_row_dials_again_with_the_same_boot_id_and_a_restarted_one_with_a_new(pair):
    first = pair.wait("master", got("hello"))["hello"]["boot_id"]
    pair.row("drop")
    pair.wait("master", lambda e: e["kind"] == "disconnected")
    hellos = lambda: [e["hello"]["boot_id"] for e in pair.entries("master") if got("hello")(e)]
    pair.wait("master", lambda e: len(hellos()) == 2)
    assert hellos() == [first, first]
    pair.row("restart")
    pair.wait("master", lambda e: len(hellos()) == 3)
    assert hellos()[2] != first


def test_a_released_row_stops_dialling(pair):
    pair.master("release")
    pair.wait("row", lambda e: e["kind"] == "released")
    assert pair.procs[1].wait(5) == 0
