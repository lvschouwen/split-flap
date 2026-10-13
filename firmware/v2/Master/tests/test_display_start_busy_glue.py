"""Source gate: the display reports busy from the start of its task until it
takes commands. Seen on the wall: during the start-up scan and homes the
snapshot said "not busy, showing nothing", so the wall queued the own row's
text again every 250 ms until the queue of 16 was full, and every start ran
the same frame up to 16 times."""
import re
from pathlib import Path

TASK = Path(__file__).resolve().parent.parent / "DisplayTask.cpp"


def _main() -> str:
    code = re.sub(r"//[^\n]*", "", TASK.read_text())
    return code[code.index("void displayTaskMain(void*) {"):]


def test_the_display_is_busy_before_it_touches_the_bus() -> None:
    main = _main()
    busy = main.index("local.busy = true;")
    assert main.index("snapshotPublish(local);", busy) < main.index("unitBusInit();")


def test_the_display_stays_busy_until_its_command_loop() -> None:
    main = _main()
    start_up = main[:main.index("for (;;) {")]
    idle = start_up.rindex("local.busy = false;")
    for step in ("unitBusProbe(busFacts, UNITS_AMOUNT);", "runBootHomeSequence(local, busFacts);",
                 "runReflashJob(local, busFacts, ReflashSweep::OutdatedOnly, 0);"):
        assert start_up.rindex(step) < idle, step
    assert start_up.count("local.busy = false;") == 1
    assert "snapshotPublish(local);" in start_up[idle:]
