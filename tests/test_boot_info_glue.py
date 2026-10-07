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
    jobs = (V2 / "Master" / "WallJobs.cpp").read_text()
    assert "buildBootInfoJson(json, sizeof(json), snap.lastBootInfo, job.seq);" in jobs


def test_esp01_reads_the_report_and_sends_the_slot():
    jobs = (V2 / "FollowerEsp01" / "FollowerUnitJobs.cpp").read_text()
    case = jobs[jobs.index("case FollowerOpKind::BootInfo: {"):]
    case = case[:case.index("break;")]
    assert "slot.ok = busReadBootInfo(op.addr, slot.report);" in case
    assert "bootInfoSlot = slot;" in case
    for forbidden in ("busBootUpdate", "busRebootToBootloader", "busHome",
                      "busRunBootUpdate"):
        assert forbidden not in case, f"the info op must not call {forbidden}"
    link = (V2 / "FollowerEsp01" / "FollowerLink.cpp").read_text()
    assert "buildBootInfoJson(text, BOOT_INFO_JSON_CAP, unitOpBootInfo()," in link


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


def test_update_drivers_run_the_shared_op():
    # #516/#530. The sequence, its timeouts and its grading are
    # shared/BootUpdateOp.h, natively tested against a scripted unit; each row
    # master supplies hooks only.
    drivers = {
        "S3": _driver("Master/DisplayTask.cpp", "static void execBootUpdate("),
        "ESP-01": _driver("FollowerEsp01/FollowerBus.cpp",
                          "void busRunBootUpdate("),
    }
    for name, body in drivers.items():
        assert "bootUpdateRun(hooks, " in body, name
        for own in ("bootUpdateDecide(", "bootStage1WentOffBus(",
                    "bootStage2Poll(", "BootUpdate(addr, "):
            assert own not in body, f"{name}: {own} belongs to the shared op"
    op = (V2 / "shared" / "BootUpdateOp.h").read_text()
    for call in ("bootUpdateDecide(", "bootStage1WentOffBus(",
                 "bootStage2Poll(info, resultBeforeSend)",
                 "maintReasonForBootFailure("):
        assert call in op, call
    assert "BOOT_RESULT_REFUSED" not in op, "judge through the helpers"


def test_start_probes_cannot_pin_the_bootloader():
    # The probes run inside the inhibit window; that is only safe while the
    # report opcode is not one of twiboot's pinning first bytes.
    op = (V2 / "shared" / "BootUpdateOp.h").read_text()
    assert "static_assert(SFP_CMD_GET_BOOT_INFO > 0x02," in op
