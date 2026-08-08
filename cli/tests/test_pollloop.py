"""Unit tests for the shared poll-loop skeleton (#441 finding 1b/2): every
background poller in the TUI now runs through this, so a bug here is a bug
everywhere. The property under test is exactly what #436's flood exposed —
a cycle that raises must not kill the loop, whatever the exception type."""
from __future__ import annotations

import threading

from splitflap_tui.pollloop import run_poll_loop


def test_cycle_exception_is_reported_and_loop_keeps_running():
    stop_event = threading.Event()
    calls = []
    errors = []

    def cycle_fn():
        calls.append(len(calls) + 1)
        if len(calls) == 1:
            raise ValueError("boom — e.g. a markup-parsing bug")
        if len(calls) >= 3:
            stop_event.set()

    run_poll_loop(stop_event, 0.0, cycle_fn, errors.append)

    assert calls == [1, 2, 3]          # cycle 1 raised; 2 and 3 still ran
    assert len(errors) == 1
    assert isinstance(errors[0], ValueError)


def test_non_splitflap_exception_types_are_all_caught():
    """The whole point of #441: a MarkupError (or any other bug), not just
    SplitflapError, must be caught — this loop has no opinion on exception
    hierarchy, it catches Exception."""
    stop_event = threading.Event()
    errors = []

    class SomeInternalBug(Exception):
        pass

    def cycle_fn():
        stop_event.set()
        raise SomeInternalBug("not a SplitflapError")

    run_poll_loop(stop_event, 0.0, cycle_fn, errors.append)

    assert len(errors) == 1
    assert isinstance(errors[0], SomeInternalBug)


def test_on_error_raising_does_not_kill_the_loop():
    stop_event = threading.Event()
    calls = []

    def cycle_fn():
        calls.append(1)
        if len(calls) >= 2:
            stop_event.set()
        raise RuntimeError("always fails")

    def buggy_on_error(exc):
        raise KeyError("the error handler itself has a bug")

    run_poll_loop(stop_event, 0.0, cycle_fn, buggy_on_error)

    assert len(calls) == 2


def test_callable_interval_is_re_evaluated_every_iteration():
    """sse_loop's reconnect backoff needs a live read each iteration, not a
    value captured once at loop start."""
    stop_event = threading.Event()
    seen = []
    state = {"n": 0}

    def interval() -> float:
        state["n"] += 1
        return 0.0

    def cycle_fn():
        seen.append(state["n"])
        if len(seen) >= 3:
            stop_event.set()

    run_poll_loop(stop_event, interval, cycle_fn, lambda exc: None)

    assert seen == [0, 1, 2]           # interval() ran once per completed cycle


def test_stopped_before_first_cycle_never_runs_cycle_fn():
    stop_event = threading.Event()
    stop_event.set()
    calls = []

    run_poll_loop(stop_event, 0.0, lambda: calls.append(1), lambda exc: None)

    assert calls == []
