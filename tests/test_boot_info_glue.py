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
