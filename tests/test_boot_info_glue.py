"""Source gate for the read-only boot report (#499): both boards must read the
unit's report into the slot they serve, and the info op must stay read-only —
it is the one unit op an operator runs freely, before and after an update."""
from pathlib import Path

V2 = Path(__file__).resolve().parents[1] / "firmware" / "v2"


def test_s3_reads_the_report_and_serves_the_slot():
    task = (V2 / "Master" / "DisplayTask.cpp").read_text()
    exec_fn = task[task.index("static void execBootInfo("):]
    exec_fn = exec_fn[:exec_fn.index("\n}\n")]
    assert "slot.ok = unitBusReadBootInfo(cmd.unitAddress, slot.report);" in exec_fn
    assert "displayApplyBootInfoResult(local, slot);" in exec_fn
    for forbidden in ("unitBusBootUpdate", "RebootToBootloader", "unitBusHome",
                      "unitBusReboot"):
        assert forbidden not in exec_fn, f"the info op must not call {forbidden}"
    web = (V2 / "Master" / "WebMaintenance.cpp").read_text()
    assert "buildBootInfoJson(buf, sizeof(buf), snap.lastBootInfo, (uint32_t)seq);" in web


def test_esp01_reads_the_report_and_serves_the_slot():
    web = (V2 / "FollowerEsp01" / "FollowerWeb.cpp").read_text()
    case = web[web.index("case FollowerOpKind::BootInfo: {"):]
    case = case[:case.index("break;")]
    assert "slot.ok = busReadBootInfo(op.addr, slot.report);" in case
    assert "bootInfoSlot = slot;" in case
    for forbidden in ("busBootUpdate", "busRebootToBootloader", "busHome",
                      "busRunBootUpdate"):
        assert forbidden not in case, f"the info op must not call {forbidden}"
    assert "buildBootInfoJson(buf, sizeof(buf), bootInfoSlot, (uint32_t)seq);" in web


def test_unit_detects_an_unreadable_lock_and_gates_on_the_effective_byte():
    # #518. The detection and the "unknown lock proceeds" rule are pure and
    # unit-tested; this pins the sketch glue that applies them.
    ino = (V2 / "Unit" / "UnitBootUpdate.ino").read_text()
    read = ino[ino.index("static void readLockAndFuses(BootUpdateReport& r) {"):]
    read = read[:read.index("\n}\n")]
    assert "bootLockFuseReadFellThrough(lock, low, high, ext, flash0to3)" in read
    assert "pgm_read_byte(i)" in read
    assert "r.lockFuseReadable = false;" in read
    refresh = ino[ino.index("void refreshBootInfoReply() {"):]
    refresh = refresh[:refresh.index("\n}\n")]
    assert "readLockAndFuses(r);" in refresh
    assert "boot_lock_fuse_bits_get" not in refresh, "one reader, so one detection"
    run = ino[ino.index("void runBootUpdate(uint8_t stage) {"):]
    run = run[:run.index("\n}\n")]
    assert "bootRunStage(stage, bootEffectiveLockByte(lockFuses));" in run
    assert ino.count("boot_lock_fuse_bits_get(") == 4, "all reads live in readLockAndFuses"


def _driver(path, signature):
    src = (V2 / path).read_text()
    body = src[src.index(signature):]
    return body[:body.index("\n}\n")]


def test_update_drivers_use_the_shared_decisions():
    # #516. The decisions are pure and unit-tested in BootUpdatePlan.h; the two
    # drivers are bus glue no native test reaches, so pin what they call and in
    # which order.
    drivers = {
        "S3": (_driver("Master/DisplayTask.cpp", "static void execBootUpdate("),
               "unitBusWaitBatchIdle(&addr, 1, BOOT_UPDATE_IDLE_MS);",
               "unitBusBootUpdate(addr, 1)", "unitBusBootUpdate(addr, 2)",
               "unitBusHome(addr)"),
        "ESP-01": (_driver("FollowerEsp01/FollowerBus.cpp", "void busRunBootUpdate("),
                   "waitForBatchIdle(&addr, 1, 8000);",
                   "busBootUpdate(addr, 1)", "busBootUpdate(addr, 2)",
                   "busHome(addr)"),
    }
    for name, (body, idle, stage1, stage2, home) in drivers.items():
        # The drum settles before stage 1 is requested.
        assert body.index(idle) < body.index(stage1), name
        # A unit that never left the bus is reported by what it said.
        after1 = body[body.index(stage1):body.index(stage2)]
        assert "if (!started) {" in after1, name
        assert "bootFailureReason(bootResultFailure(info.lastResult))" in after1, name
        # The stage-2-only path homes first; the poll judges through the helper
        # with the result read before the send.
        before2 = body[body.index("if (plan.needStage2) {"):body.index(stage2)]
        assert "if (!plan.needStage1) {" in before2 and home in before2, name
        assert "const uint8_t resultBeforeSend = info.lastResult;" in before2, name
        after2 = body[body.index(stage2):]
        assert "bootStage2Poll(info, resultBeforeSend)" in after2, name
        assert "MaintReason::BootUnitLost" in after2, name
        assert "BOOT_RESULT_REFUSED" not in body, f"{name}: judge through the helpers"
