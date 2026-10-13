"""Source gate for the task priorities of Tasks.cpp (#614).

The link task hands the row boards their flip instants and answers their time
requests; on core 0 it must go before the worker's TLS download and an MQTT
burst, and stay under the display task, whose flap timing wins everything.
"""
import re
from pathlib import Path

TASKS = Path(__file__).resolve().parent.parent / "Tasks.cpp"


def _priorities():
    code = re.sub(r"//[^\n]*", "", TASKS.read_text())
    values = {
        name: int(value)
        for name, value in re.findall(
            r"constexpr UBaseType_t (\w+_PRIORITY) = (\d+);", code)
    }
    created = {
        task: prio
        for task, prio in re.findall(
            r'"(\w+)", \w+_STACK, [^,]+,\s*(\w+_PRIORITY),', code)
    }
    return values, created


def test_the_link_task_goes_before_the_other_network_tasks():
    values, created = _priorities()
    link = values[created["link"]]
    for task in ("net", "mqtt", "worker"):
        assert link > values[created[task]], task


def test_the_display_task_still_wins():
    values, created = _priorities()
    assert values[created["display"]] > values[created["link"]]
