"""Source gate for the network liveness glue (#501).

NetLivenessPolicy.h decides (natively tested). What a native test cannot reach
is the wiring: that the restart stands down under a job it would cut in half,
that the own-server probe demands an answered request, and that the probes run
on the one task that is allowed to block.
"""
import re
from pathlib import Path

MASTER = Path(__file__).resolve().parent.parent


def _code(name):
    return re.sub(r"//[^\n]*", "", (MASTER / name).read_text())


def test_liveness_restart_stands_down_under_a_flash_job():
    code = _code("WifiService.cpp")
    step = code.index("netLivenessStep(")
    restart = code.index("netLivenessAddStrike()", step)
    guard = code[step:restart]
    assert "Update.isRunning()" in guard
    assert "reflashInProgress(displaySnapshotGet().reflash)" in guard
    assert "!restartWouldCutAJob" in guard
    assert "!restartPending" in code[step - 800:step]


def test_own_server_probe_needs_an_answered_request():
    code = _code("NetLiveness.cpp")
    probe = code[code.index("NetProbe tcpProbe("):code.index("void netLivenessProbeTick()")]
    # Refused may only count for the request-less (gateway) probe.
    refused = probe.index("ECONNREFUSED")
    assert "request == nullptr" in probe[refused - 200:refused]
    assert 'memcmp(head, "HTTP/", 5) == 0' in probe
    tick = code[code.index("void netLivenessProbeTick()"):]
    assert "tcpProbe(gateway, 80, nullptr, gwStep)" in tick
    assert ": tcpProbe(self, 80, kRequest, ownStep)" in tick
    assert re.search(r'kRequest\[\] =\s*"GET ', tick)


def test_probes_run_on_the_worker_task_only():
    tasks = _code("Tasks.cpp")
    body = tasks[tasks.index("static void workerTaskMain"):]
    body = body[:body.index("\n}\n")]
    assert "netLivenessProbeTick();" in body
    for name in ("WifiService.cpp", "WebEndpoints.cpp", "MqttService.cpp",
                 "DisplayTask.cpp"):
        assert "netLivenessProbeTick" not in _code(name), name


