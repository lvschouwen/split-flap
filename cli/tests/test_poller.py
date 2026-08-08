"""Poller thread-lifecycle tests: stop() must abort an in-flight SSE read
promptly, not just set a flag the read loop won't notice until it returns
on its own. call_from_thread happens on a live app only (see app.py's
on_unmount comment) — an SSE thread stuck past unmount is a real hazard,
not just slow shutdown."""
from __future__ import annotations

import threading
import time

from splitflap_client.transport import Unreachable
from splitflap_tui.poller import Poller
import splitflap_tui.poller as poller_module


class _FakeApp:
    def call_from_thread(self, fn, *args):
        fn(*args)

    def apply_wall_stale(self) -> None:
        pass

    def apply_display(self, event) -> None:
        pass


class _BlockingSseClient:
    """Stands in for BoardClient: stream()'s read blocks until closed."""

    def __init__(self, url: str):
        self.url = url
        self.closed_event = threading.Event()

    def __enter__(self) -> "_BlockingSseClient":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def close(self) -> None:
        self.closed_event.set()


def test_stop_aborts_inflight_sse_read_promptly(monkeypatch):
    def fake_display_events(client):
        if not client.closed_event.wait(timeout=5.0):
            raise AssertionError("stop() never closed the SSE client")
        raise Unreachable("http://x/events", RuntimeError("closed by stop()"))

    monkeypatch.setattr(poller_module, "display_events", fake_display_events)

    p = Poller(_FakeApp(), _BlockingSseClient, "http://x",
               poll_s=100, log_poll_s=100)
    t = threading.Thread(target=p.sse_loop, daemon=True)
    t.start()
    time.sleep(0.1)          # let sse_loop enter the blocking read

    start = time.monotonic()
    p.stop()
    t.join(timeout=2.0)
    elapsed = time.monotonic() - start

    assert not t.is_alive(), "SSE thread still blocked after stop()"
    assert elapsed < 2.0
