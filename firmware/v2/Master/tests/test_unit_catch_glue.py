"""Source gate for the power-on catch (#554).

UnitCatchPolicy.h decides (natively tested). What a native test cannot reach:
the questions have to be among the very first things this board does — one second
after power arrives the unit has left its bootloader — and they are the one
bus call made outside displayTask, so nothing else may grow next to them.
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


def test_the_unit_is_asked_before_anything_else_in_setup():
    setup = _body(_code("main.cpp"), "void setup()")
    statements = [s.strip() for s in setup.split(";") if s.strip()]
    # Only what must come first comes first: the breadcrumbs of the start
    # before, and the boot guard, so a start that dies in the catch is counted.
    assert statements[:6] == ["Serial.begin(115200)", "rebootCauseConsume()",
                              "bootTraceInit()", "crashCtxBoot()", "bootGuardBoot()",
                              "const uint32_t catchMs = unitCatchAtPowerOn()"]
    # The time it took comes off the start-up delay; it is never added to it.
    assert "if (catchMs < 2000) delay(2000 - catchMs)" in setup
    assert setup.index("unitCatchAtPowerOn()") < setup.index("delay(")
    assert setup.index("bootGuardLogReport();") < setup.index("unitCatchLogReport();")


def test_the_early_bus_call_has_one_caller_and_leaves_no_breadcrumb():
    for path in sorted(ROOT.glob("*.cpp")):
        if path.name in ("UnitBus.cpp", "UnitCatch.cpp"):
            continue
        assert "unitBusCatchAtPowerOn" not in _code(path.name), path.name
    early = _body(_code("UnitBus.cpp"), "bool unitBusCatchAtPowerOn(")
    assert "EarlyTwibootBus bus;" in early
    assert "twibootIsBootloader(bus," in early
    assert "crashCtxMark" not in early and "isUnitInBootloader" not in early
    assert early.rstrip().endswith("return caught;")
    assert "Wire.end();" in early  # displayTask brings the bus up itself
    adapter = _body(_code("UnitBus.cpp"), "struct EarlyTwibootBus")
    assert "crashCtxMark" not in adapter and "recoverBusAfterFailedRead" not in adapter


def test_the_armed_address_survives_a_power_cycle():
    code = _code("UnitCatch.cpp")
    assert 'kNamespace = "sfboot"' in code
    assert "RTC_NOINIT" not in code  # RTC memory is gone with the power
    arm = _body(code, "void unitCatchArm(")
    assert "store(addr);" in arm and "unitCatchAddressValid(" in arm


def test_a_caught_unit_is_flashed_at_start_whatever_the_update_setting():
    task = _code("DisplayTask.cpp")
    caught = task.index("unitCatchAfterProbe(armedAddr, busFacts[armedIndex]) == UnitCatchStep::Flash")
    flash = task.index("runReflashJob(local, busFacts, ReflashSweep::ForcedOne, armedAddr);")
    setting = task.index("if (!tasksReflashOnBoot()) {")
    assert caught < flash < setting
    assert task.index("UnitCatchStep::Disarm") < setting
    job = _body(task, "static void runReflashJob(")
    assert "unitCatchAfterForcedRun(" in job and "unitCatchArm(toCatch);" in job
    # A run that brought the armed unit back ends the catch there and then.
    reprobe = job.index("unitBusProbe(busFacts, UNITS_AMOUNT);")
    assert reprobe < job.index("unitCatchDisarm();")
