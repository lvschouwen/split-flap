"""Source gates for the update from a release (#583).

The order of an install is natively tested (test_release_fetch). What a
native test cannot reach: that the job and the uploads never write the same
slot at once. The job feeds the three writers the upload routes feed, so each
of those routes, and everything else that restarts the board or occupies its
units, has to stand down while it runs.
"""
import re
from pathlib import Path

MASTER = Path(__file__).resolve().parent.parent


def _code(name):
    return re.sub(r"//[^\n]*", "", (MASTER / name).read_text())


def _route(code, path, method):
    start = code.index(f'"{path}", {method}')
    end = code.find("server.on(", start)
    return code[start:end if end > 0 else len(code)]


def test_no_upload_begins_while_an_update_from_a_release_runs():
    code = _code("WebFirmware.cpp")
    for path, begins in (("/firmware/master", "Update.begin("),
                         ("/firmware/rescue", "factoryWriteBegin("),
                         ("/firmware/row", "followerImageWriteBegin(")):
        route = _route(code, path, "HTTP_POST")
        assert "releaseUpdateRunning()" in route, path
        # Refused before the writer is touched.
        assert route.index("releaseUpdateRunning()") < route.index(begins), path
    boot = _route(code, "/firmware/rescue-boot", "HTTP_POST")
    assert boot.index("releaseUpdateRunning()") < boot.index("rescueBootArm()")


def test_an_update_is_not_started_over_an_upload_or_a_unit_update():
    code = _code("WebWall.cpp")
    handler = code[code.index("void handleRelease("):]
    handler = handler[:handler.index("\n}\n")]
    ask = handler.index("releaseAskUpdate(")
    for gate in ("reflashInProgress(", "wallUnitUpdateRunning()", "webFirmwareUploadActive()"):
        assert handler.index(gate) < ask, gate
    active = _code("WebFirmware.cpp")
    active = active[active.index("bool webFirmwareUploadActive()"):]
    active = active[:active.index("}")]
    for writer in ("otaOwnerRequest", "factoryInstallInProgress()", "rowImageOwnerRequest"):
        assert writer in active, writer


def test_nothing_restarts_the_board_or_starts_a_unit_job_under_an_update():
    code = _code("WebEndpoints.cpp")
    refusal = code[code.index("const char* webRestartRefusal()"):]
    assert "releaseUpdateRunning()" in refusal[:refusal.index("\n}\n")]
    jobs = _code("WebWall.cpp")
    job = jobs[jobs.index("void handleJob("):]
    job = job[:job.index("\n}\n")]
    assert job.index("releaseUpdateRunning()") < job.index("startOwnJob(")


def test_the_job_runs_on_the_worker_and_is_told_of_a_changed_setting():
    worker = _code("Tasks.cpp")
    worker = worker[worker.index("static void workerTaskMain("):]
    assert "releaseTick();" in worker[:worker.index("\n}\n")]
    assert "releaseSetSettings(" in _code("WebEndpoints.cpp")
    main = _code("main.cpp")
    assert main.index("releaseInit(") < main.index("tasksInit(")


def test_the_update_from_a_release_is_in_the_event_record():
    assert re.search(r'\{"update-from-release",\s*\d+\}', _code("WallJobs.h"))
    # A job's name has to fit the job table.
    size = int(re.search(r"char name\[(\d+)\]", _code("WallOps.h")).group(1))
    assert len("update-from-release") < size
