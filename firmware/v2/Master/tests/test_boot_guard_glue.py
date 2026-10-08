"""Source gate for the boot guard's glue (#281).

BootGuardPolicy.h decides (natively tested); BootGuard.cpp has to keep the
count in reset-surviving memory, arm the rescue boot only behind a verified
rescue image, and be called where a crashing image still reaches it. None of
that is reachable from a native test, and getting it wrong either never trips
or points a wall-mounted board at a slot that does not start.
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def _code(name):
    return re.sub(r"//[^\n]*", "", (ROOT / name).read_text())


def _body(code, signature):
    start = code.index(signature)
    i = code.index("{", start)
    depth = 0
    for j in range(i, len(code)):
        depth += {"{": 1, "}": -1}.get(code[j], 0)
        if depth == 0:
            return code[i + 1:j]
    raise AssertionError(signature)


def test_count_lives_in_reset_surviving_memory():
    assert "RTC_NOINIT_ATTR static BootGuardRecord guardRecord;" in _code("BootGuard.cpp")


def test_boot_counts_this_reset_and_writes_it_back_before_deciding():
    boot = _body(_code("BootGuard.cpp"), "void bootGuardBoot(")
    step = boot.index("bootGuardStep(bootGuardDecode(guardRecord), reason)")
    write = boot.index("bootGuardEncode(guardRecord, crashes);")
    assert step < write < boot.index("bootGuardShouldTrip(crashes)")


def test_rescue_is_armed_only_behind_a_verified_image():
    boot = _body(_code("BootGuard.cpp"), "void bootGuardBoot(")
    verify = boot.index("if (!factorySlotImageVerified())")
    assert "return;" in boot[verify:boot.index("rescueBootArm()")]
    assert verify < boot.index("rescueBootArm()")
    # The only arm in the file.
    assert len(re.findall(r"rescueBootArm\(", _code("BootGuard.cpp"))) == 1


def test_a_failed_arm_neither_notes_a_trip_nor_restarts():
    boot = _body(_code("BootGuard.cpp"), "void bootGuardBoot(")
    arm = boot.index("if (!rescueBootArm()) return;")
    assert arm < boot.index("saveTrip(") < boot.index("esp_restart();")
    # The count is cleared for the image that starts after the rescue one.
    assert "bootGuardEncode(guardRecord, 0);" in boot[arm:boot.index("esp_restart();")]


def test_verification_reads_the_whole_image_the_way_the_bootloader_does():
    body = _body(_code("FactorySlot.cpp"), "bool factorySlotImageVerified(")
    assert "esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &pos, &meta) == ESP_OK" in body


def test_guard_runs_before_any_init_that_can_crash():
    setup = _body(_code("main.cpp"), "void setup(")
    guard = setup.index("bootGuardBoot();")
    assert guard < setup.index("webLogInit();")
    assert guard < setup.index("flashLogInit();")
    assert guard < setup.index("settingsStore.begin();")


def test_a_running_net_task_forgives_and_nothing_else_does():
    assert "bootGuardTick();" in _body(_code("Tasks.cpp"), "static void netTaskMain(")
    tick = _body(_code("BootGuard.cpp"), "void bootGuardTick(")
    assert "!bootGuardHealthy(millis())" in tick
    # Three writes: this boot's count, the trip, the healthy run.
    assert len(re.findall(r"bootGuardEncode\(guardRecord", _code("BootGuard.cpp"))) == 3
