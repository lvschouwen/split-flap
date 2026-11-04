"""Source gate for the flash log's early flush (#628).

FlashLogPolicy.h decides when a warning or an error flushes early (natively
tested). FlashLog.cpp has to keep the producer off the flash: a line staged
from any task only raises a flag under the stage mutex, and only netTask's
tick hands that flag to the policy and clears it with the bytes it takes.
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


def test_staging_never_touches_the_flash():
    stage = _body(_code("FlashLog.cpp"), "void flashLogStage(")
    assert "LittleFS" not in stage
    assert "flashLogTick" not in stage


def test_staging_classifies_outside_the_lock_and_raises_the_flag_inside():
    stage = _body(_code("FlashLog.cpp"), "void flashLogStage(")
    classify = stage.index("flashLogTextUrgent(data, len)")
    take = stage.index("xSemaphoreTake(stageMutex")
    raise_flag = stage.index("stage.urgent = true")
    give = stage.index("xSemaphoreGive(stageMutex)")
    assert classify < take < raise_flag < give


def test_tick_hands_the_flag_to_the_policy_and_clears_it_with_the_bytes():
    tick = _body(_code("FlashLog.cpp"), "void flashLogTick(")
    take = tick.index("xSemaphoreTake(stageMutex")
    decide = tick.index("stage.urgent))")
    clear = tick.index("stage.urgent = false")
    give = tick.index("xSemaphoreGive(stageMutex)")
    assert take < decide < clear < give
    assert tick.index("stage.used = 0") < clear
