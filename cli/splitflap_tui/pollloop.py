"""Shared background poll-loop skeleton (#441 finding 1b).

Before this existed, poller.py had three copies of "while not stopped: try:
do a cycle except SplitflapError: report; wait" plus a fourth, near-identical
copy in BoardDetailScreen._poll. The narrow `except SplitflapError` was the
#441 root cause: a markup-parsing bug in a rendering call reached via
call_from_thread (board text containing "[/]") raised textual.markup.
MarkupError, which is NOT a SplitflapError, escaped that narrow except, and
permanently killed the poller thread with no stale marker — 425 of 429
flash-log lines were one flapping signal, evicting all forensics (see
project memory on #436). A rendering bug must never silently kill a
background thread again, so this helper catches Exception, not just the
transport's own error hierarchy.
"""
from __future__ import annotations

import threading
from typing import Callable, TypeVar, Union

T = TypeVar("T")


def run_poll_loop(stop_event: threading.Event,
                   interval: Union[float, Callable[[], float]],
                   cycle_fn: Callable[[], None],
                   on_error: Callable[[BaseException], None]) -> None:
    """Run cycle_fn() once per iteration until stop_event is set.

    Any exception cycle_fn raises (not just SplitflapError) is caught and
    handed to on_error — the loop keeps going either way. on_error itself is
    also guarded: a bug in the error-reporting path must not kill the loop
    either.

    interval is either a fixed wait (float) or a zero-arg callable invoked
    after each cycle to get that iteration's wait — sse_loop's reconnect
    backoff grows on failure and resets on success, so it needs to read
    whatever cycle_fn just mutated rather than a constant.

    cycle_fn and on_error are responsible for their own
    `if stop_event.is_set(): return` guard immediately before any
    call_from_thread — this loop cannot see inside a cycle already in
    flight when stop() fires (finding 2: a stop-triggered disconnect must
    not still schedule a callback onto a torn-down app).
    """
    while not stop_event.is_set():
        try:
            cycle_fn()
        except Exception as exc:            # noqa: BLE001 - see module docstring
            try:
                on_error(exc)
            except Exception:
                pass          # the error handler itself must never kill the loop
        wait_s = interval() if callable(interval) else interval
        stop_event.wait(wait_s)
