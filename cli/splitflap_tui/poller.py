from __future__ import annotations

import threading
from typing import Callable

from splitflap_client.events import display_events
from splitflap_client.logs import fetch_flash_log
from splitflap_client.models import ClusterStatus, StatusAggregate
from splitflap_client.transport import BoardClient, SplitflapError

from .pollloop import run_poll_loop


def _internal_error(exc: BaseException) -> str:
    # #441 finding 1b/2: a non-SplitflapError exception (e.g. a markup bug
    # reached through call_from_thread) is a code bug, not a connectivity
    # event — surfaced distinctly so it's never mistaken for one.
    return f"INTERNAL ERROR — {exc!r}"


class Poller:
    """Leader-only background polling. Threads, not asyncio: BoardClient is
    sync httpx, and Textual's call_from_thread is the documented bridge.

    Every loop below now runs through pollloop.run_poll_loop (#441 finding
    1b) so a bug inside a cycle — including one only reachable through
    call_from_thread, like a markup-parsing crash — can never silently kill
    the thread. Every call_from_thread call is guarded by a stop_event check
    immediately beforehand (finding 2): stop()/unmount can race an in-flight
    cycle, and a callback must never be scheduled onto a torn-down app."""

    def __init__(self, app, client_factory: Callable[[str], BoardClient],
                 leader_url: str, poll_s: float, log_poll_s: float):
        self.app = app
        self.factory = client_factory
        self.url = leader_url
        self.poll_s = poll_s
        self.log_poll_s = log_poll_s
        self.stop_event = threading.Event()
        self._sse_client: BoardClient | None = None
        self._sse_backoff = 1.0

    def start(self) -> None:
        for fn in (self.poll_status, self.poll_log, self.sse_loop):
            threading.Thread(target=fn, daemon=True).start()

    def stop(self) -> None:
        self.stop_event.set()
        # sse_loop's read has no timeout (SSE is long-lived by design) and
        # won't notice stop_event until it returns on its own -> close the
        # in-flight client to abort the blocked read now, not whenever the
        # board next talks. A thread left running past unmount would call
        # app.call_from_thread on a torn-down app.
        c = self._sse_client
        if c:
            try:
                c.close()
            except Exception:
                pass

    # ---- /status + /cluster/status ----
    def _status_cycle(self) -> None:
        with self.factory(self.url) as c:
            agg = StatusAggregate.from_json(c.get_json("/status"))
            cluster = ClusterStatus.from_json(c.get_json("/cluster/status"))
        if self.stop_event.is_set():
            return
        self.app.call_from_thread(self.app.apply_status, agg, cluster)

    def _status_error(self, exc: BaseException) -> None:
        if self.stop_event.is_set():
            return
        message = str(exc) if isinstance(exc, SplitflapError) else _internal_error(exc)
        self.app.call_from_thread(self.app.apply_disconnect, message)

    def poll_status(self) -> None:
        run_poll_loop(self.stop_event, self.poll_s,
                      self._status_cycle, self._status_error)

    # ---- flash log tail ----
    def _log_cycle(self) -> None:
        with self.factory(self.url) as c:
            text = fetch_flash_log(c)
        tail = text.splitlines()[-200:]
        if self.stop_event.is_set():
            return
        self.app.call_from_thread(self.app.apply_log, tail)

    def _log_error(self, exc: BaseException) -> None:
        if isinstance(exc, SplitflapError):
            return                   # status poller owns the banner
        if self.stop_event.is_set():
            return
        self.app.call_from_thread(self.app.apply_disconnect, _internal_error(exc))

    def poll_log(self) -> None:
        run_poll_loop(self.stop_event, self.log_poll_s,
                      self._log_cycle, self._log_error)

    # ---- /events SSE ----
    def _sse_cycle(self) -> None:
        try:
            with self.factory(self.url) as c:
                self._sse_client = c
                try:
                    for event in display_events(c):
                        self._sse_backoff = 1.0
                        if self.stop_event.is_set():
                            return
                        self.app.call_from_thread(self.app.apply_display, event)
                finally:
                    self._sse_client = None
        finally:
            if not self.stop_event.is_set():
                self.app.call_from_thread(self.app.apply_wall_stale)

    def _sse_error(self, exc: BaseException) -> None:
        if isinstance(exc, SplitflapError):
            return          # normal reconnect path; the finally above already
                             # marked the wall stale
        if self.stop_event.is_set():
            return
        self.app.call_from_thread(self.app.apply_disconnect, _internal_error(exc))

    def _sse_interval(self) -> float:
        wait_s = self._sse_backoff
        self._sse_backoff = min(wait_s * 2, 30.0)
        return wait_s

    def sse_loop(self) -> None:
        run_poll_loop(self.stop_event, self._sse_interval,
                      self._sse_cycle, self._sse_error)
