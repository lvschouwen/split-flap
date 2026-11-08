"""Source gate for the brownout-loop guard and the low-heap watch (#504).

The rules are natively tested (test_brownout_guard, test_heap_watch); this
pins where the glue runs: the record before any task, the hold before the
display moves a unit, the ticks on netTask, the clear as an API action, and a
heap watch that never blocks the watched task it runs on.
"""
import re
from pathlib import Path

MASTER = Path(__file__).resolve().parent.parent


def _code(name: str) -> str:
    text = (MASTER / name).read_text(encoding="utf-8")
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)
    return re.sub(r"//[^\n]*", "", text)


def _body(code: str, signature: str) -> str:
    start = code.index(signature)
    depth, i = 0, code.index("{", start)
    for j in range(i, len(code)):
        depth += {"{": 1, "}": -1}.get(code[j], 0)
        if depth == 0:
            return code[i:j + 1]
    raise AssertionError(signature)


def test_setup_counts_the_reset_before_any_task_exists() -> None:
    setup = _body(_code("main.cpp"), "void setup() {")
    for call in ("brownoutGuardBoot();", "heapWatchBoot();"):
        assert setup.index("bootTraceInit();") < setup.index(call) < setup.index("tasksInit("), call
    for call in ("brownoutGuardLogReport();", "heapWatchLogReport();"):
        assert setup.index("flashLogInit();") < setup.index(call), call


def test_the_hold_comes_after_the_probe_and_before_any_unit_moves() -> None:
    main = _body(_code("DisplayTask.cpp"), "void displayTaskMain(void*) {")
    start_up = main[:main.index("for (;;) {")]
    hold = start_up.index("brownoutGuardHoldDisplay();")
    assert start_up.index("unitBusProbe(busFacts, UNITS_AMOUNT);") < hold
    for step in ("catchArmedUnitAtStart(", "runBootHomeSequence(", "runReflashJob(", "local.busy = false;"):
        assert hold < start_up.index(step), step
    assert start_up.count("brownoutGuardHoldDisplay();") == 1


def test_the_hold_feeds_the_task_watchdog_while_it_waits() -> None:
    hold = _body(_code("BrownoutGuard.cpp"), "void brownoutGuardHoldDisplay() {")
    loop = hold[hold.index("while (held.load())"):]
    assert "wdtFeed();" in loop and "delay(" in loop
    assert "unitBusSetMotionCeiling(BROWNOUT_HOLD_MOTION_CAP);" in hold


def test_nvs_is_written_by_setup_and_net_task_only() -> None:
    code = _code("BrownoutGuard.cpp")
    for name in ("bool brownoutGuardRequestClear() {", "void brownoutGuardHoldDisplay() {",
                 "String brownoutGuardJson() {", "bool brownoutGuardHeld() {"):
        assert "persist()" not in _body(code, name), name


def test_net_task_ticks_both() -> None:
    net = _body(_code("Tasks.cpp"), "static void netTaskMain(void* arg) {")
    loop = net[net.index("for (;;) {"):]
    assert "brownoutGuardTick();" in loop
    assert "heapWatchTick();" in loop


def test_the_heap_watch_never_waits() -> None:
    code = _code("HeapWatch.cpp")
    assert "delay(" not in code and "vTaskDelay" not in code and "portMAX_DELAY" not in code
    assert "heap_caps_get_free_size(MALLOC_CAP_INTERNAL)" in code
    # The graceful reboot stamps the cause the next boot reports.
    assert "webRequestReboot(cause);" in code
    assert "ESP.restart" not in code and "esp_restart" not in code


def test_the_clear_is_an_action_and_the_state_is_in_the_board_facts() -> None:
    wall = _code("WebWall.cpp")
    assert 'strcmp(name, "clear-brownout-hold") == 0' in wall
    board = _code("WebBoard.cpp")
    assert 'root["brownoutGuard"] = serialized(brownoutGuardJson());' in board
    assert 'root["heapWatch"] = serialized(heapWatchJson());' in board
    assert "clear-brownout-hold" in (MASTER / "ApiIndex.h").read_text(encoding="utf-8")
    watch = _code("WallWatch.cpp")
    assert "facts.brownoutHold = brownoutGuardHeld();" in watch
