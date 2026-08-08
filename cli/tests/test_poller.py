"""Poller thread-lifecycle tests: stop() must abort an in-flight SSE read
promptly, not just set a flag the read loop won't notice until it returns
on its own. call_from_thread happens on a live app only (see app.py's
on_unmount comment) — an SSE thread stuck past unmount is a real hazard,
not just slow shutdown."""
from __future__ import annotations

import threading
import time

import httpx

from splitflap_client.transport import BoardClient, Unreachable
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


# ---- #441 finding 2: a cycle already in flight when stop() fires must not
# schedule call_from_thread once it completes.

class _RecordingApp:
    def __init__(self):
        self.calls: list[tuple] = []

    def call_from_thread(self, fn, *args):
        self.calls.append((fn, args))
        fn(*args)

    def apply_status(self, *a) -> None:
        pass

    def apply_disconnect(self, *a) -> None:
        pass


def test_poll_status_skips_call_from_thread_scheduled_after_stop():
    """The /status fetch is in flight (blocked in the mock transport
    handler) when stop() fires; once it completes, poll_status's cycle must
    see stop_event set and skip apply_status entirely — no callback may be
    scheduled onto a torn-down app post-stop."""
    entered = threading.Event()
    release = threading.Event()

    def handler(req):
        if req.url.path == "/status":
            entered.set()
            assert release.wait(5.0), "test never released the blocked GET"
        return httpx.Response(200, json={})

    factory = lambda url: BoardClient(url, transport=httpx.MockTransport(handler))

    app = _RecordingApp()
    p = Poller(app, factory, "http://x", poll_s=100, log_poll_s=100)
    t = threading.Thread(target=p.poll_status, daemon=True)
    t.start()
    assert entered.wait(2.0), "poll_status never reached the blocking GET"

    p.stop()
    release.set()
    t.join(timeout=2.0)

    assert not t.is_alive()
    assert app.calls == [], "call_from_thread was scheduled after stop()"


def test_sse_loop_skips_apply_wall_stale_after_stop_triggered_disconnect(monkeypatch):
    """The concrete race named in this module's/poller.py's docstrings:
    sse_loop used to call apply_wall_stale unconditionally after ANY
    disconnect, including one stop() itself triggered by closing the
    in-flight SSE client. stop() sets stop_event BEFORE closing the client
    (see Poller.stop), so by the time the resulting exception reaches
    sse_loop's finally block, stop_event is already set — apply_wall_stale
    (and every other call_from_thread in that path) must be skipped."""
    def fake_display_events(client):
        if not client.closed_event.wait(timeout=5.0):
            raise AssertionError("stop() never closed the SSE client")
        raise Unreachable("http://x/events", RuntimeError("closed by stop()"))

    monkeypatch.setattr(poller_module, "display_events", fake_display_events)

    app = _RecordingApp()
    p = Poller(app, _BlockingSseClient, "http://x", poll_s=100, log_poll_s=100)
    t = threading.Thread(target=p.sse_loop, daemon=True)
    t.start()
    time.sleep(0.1)          # let sse_loop enter the blocking read

    p.stop()
    t.join(timeout=2.0)

    assert not t.is_alive(), "SSE thread still blocked after stop()"
    assert app.calls == [], "call_from_thread was scheduled after stop()"


# ---- #452: client-handoff lock + status-loop client reuse

class _CloseTrackingClient:
    def __init__(self):
        self.closed = threading.Event()

    def close(self):
        self.closed.set()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


class _NullApp:
    def call_from_thread(self, fn, *a):
        fn(*a)


def test_sse_client_created_after_stop_is_closed_immediately():
    """#452: the stop()-vs-reconnect TOCTOU — stop() fired while a fresh
    cycle was between factory() and the _sse_client assignment used to leak
    a client blocked in a long read past unmount."""
    client = _CloseTrackingClient()
    p = Poller(_NullApp(), lambda url: client, "http://x", 1.0, 1.0)
    p.stop()                     # stop FIRST
    p._sse_cycle()               # cycle races in afterwards
    assert client.closed.is_set()
    assert p._sse_client is None


def test_status_loop_reuses_one_client_and_retries_once(monkeypatch):
    made = []

    class _JsonClient(_CloseTrackingClient):
        def __init__(self, fail_first: bool):
            super().__init__()
            self.fail_first = fail_first

        def get_json(self, path):
            if self.fail_first:
                self.fail_first = False
                raise Unreachable("http://x" + path, OSError("keepalive"))
            if path == "/status":
                return {"settings": {"plat": "esp32s3"}}
            return {"enabled": False}

    def factory(url):
        c = _JsonClient(fail_first=(len(made) == 0))
        made.append(c)
        return c

    applied = []

    class _App:
        def call_from_thread(self, fn, *a):
            applied.append(fn.__name__)

        def apply_status(self, agg, cluster): ...
        def apply_disconnect(self, msg): ...

    app = _App()
    p = Poller(app, factory, "http://x", 1.0, 1.0)
    p._status_cycle()            # first client dies mid-flight -> retried fresh
    p._status_cycle()            # second cycle reuses the fresh client
    assert len(made) == 2        # NOT 3: retry made one, cycle 2 made none
    assert applied == ["apply_status", "apply_status"]   # no disconnect flash

