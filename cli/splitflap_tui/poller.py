from __future__ import annotations

import threading
import time
from typing import Callable

from splitflap_client.events import display_events
from splitflap_client.logs import fetch_flash_log
from splitflap_client.models import ClusterStatus, StatusAggregate
from splitflap_client.transport import BoardClient, SplitflapError


class Poller:
    """Leader-only background polling. Threads, not asyncio: BoardClient is
    sync httpx, and Textual's call_from_thread is the documented bridge."""

    def __init__(self, app, client_factory: Callable[[str], BoardClient],
                 leader_url: str, poll_s: float, log_poll_s: float):
        self.app = app
        self.factory = client_factory
        self.url = leader_url
        self.poll_s = poll_s
        self.log_poll_s = log_poll_s
        self.stop_event = threading.Event()
        self._sse_client: BoardClient | None = None

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

    def poll_status(self) -> None:
        while not self.stop_event.is_set():
            try:
                with self.factory(self.url) as c:
                    agg = StatusAggregate.from_json(c.get_json("/status"))
                    cluster = ClusterStatus.from_json(c.get_json("/cluster/status"))
                self.app.call_from_thread(self.app.apply_status, agg, cluster)
            except SplitflapError as exc:
                self.app.call_from_thread(self.app.apply_disconnect, str(exc))
            self.stop_event.wait(self.poll_s)

    def poll_log(self) -> None:
        while not self.stop_event.is_set():
            try:
                with self.factory(self.url) as c:
                    text = fetch_flash_log(c)
                tail = text.splitlines()[-200:]
                self.app.call_from_thread(self.app.apply_log, tail)
            except SplitflapError:
                pass                      # status poller owns the banner
            self.stop_event.wait(self.log_poll_s)

    def sse_loop(self) -> None:
        backoff = 1.0
        while not self.stop_event.is_set():
            try:
                with self.factory(self.url) as c:
                    self._sse_client = c
                    try:
                        for event in display_events(c):
                            backoff = 1.0
                            self.app.call_from_thread(self.app.apply_display, event)
                    finally:
                        self._sse_client = None
            except SplitflapError:
                pass
            self.app.call_from_thread(self.app.apply_wall_stale)
            self.stop_event.wait(backoff)
            backoff = min(backoff * 2, 30.0)
