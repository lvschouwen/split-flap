from __future__ import annotations

import threading
from typing import Callable

from textual.app import ComposeResult
from textual.screen import Screen
from textual.widgets import Footer, Header, Static

from splitflap_client.capability import PLAT_ESP01
from splitflap_client.logs import fetch_follower_log
from splitflap_client.models import ClusterHealth, Settings, UnitsHealth
from splitflap_client.transport import BoardClient, SplitflapError

from ..config import Board
from ..pollloop import run_poll_loop

POLL_S = 5.0        # floor — never poll a follower faster (esp01 superloop)

LOG_CAP_LINES = 200


def cap_log(text: str) -> str:
    return "\n".join(text.splitlines()[-LOG_CAP_LINES:])


class BoardDetailScreen(Screen):
    BINDINGS = [("escape", "app.pop_screen", "back")]

    def __init__(self, board: Board, client_factory: Callable[[str], BoardClient]):
        super().__init__()
        self.board = board
        self.factory = client_factory
        self.stop_event = threading.Event()
        self.log_cursor = 0
        self._log_text = ""
        self._client: BoardClient | None = None

    def compose(self) -> ComposeResult:
        # markup=False (#441 finding 1a): every one of these renders
        # board-supplied text (settings, unit faults, cluster health,
        # follower log lines) verbatim, not as Rich console markup.
        yield Header()
        yield Static(id="detail-settings", markup=False)
        yield Static(id="detail-units", markup=False)
        yield Static(id="detail-cluster", markup=False)
        yield Static(id="detail-log", markup=False)
        yield Footer()

    def on_mount(self) -> None:
        threading.Thread(target=self._poll, daemon=True).start()

    def on_unmount(self) -> None:
        self.stop_event.set()
        self._drop_client()

    def _poll(self) -> None:
        # #441 finding 1b: runs through the shared skeleton so a rendering
        # bug in _apply (reached via call_from_thread) can't silently kill
        # this thread the way the flood in #436 did to poller.py's loops.
        try:
            run_poll_loop(self.stop_event, POLL_S, self._cycle, self._report_error)
        finally:
            self._drop_client()

    def _fetch(self):
        # #452: one client held across cycles (was `with self.factory(...)`
        # per cycle) — a board-closed keep-alive is not an outage, so retry
        # once on a fresh client before reporting a false disconnect.
        if self._client is None:
            self._client = self.factory(self.board.url)
        c = self._client
        settings = Settings.from_json(c.get_json("/settings"))
        units = UnitsHealth.from_json(c.get_json("/units/health"))
        health = ClusterHealth.from_json(c.get_json("/cluster/health"))
        log_text = None
        if settings.plat == PLAT_ESP01:
            delta = fetch_follower_log(c, after=self.log_cursor)
            self.log_cursor = delta.cursor
            log_text = delta.text
        return settings, units, health, log_text

    def _drop_client(self) -> None:
        c, self._client = self._client, None
        if c:
            try:
                c.close()
            except Exception:
                pass

    def _cycle(self) -> None:
        try:
            settings, units, health, log_text = self._fetch()
        except SplitflapError:
            self._drop_client()
            if self.stop_event.is_set():      # bail before opening a fresh
                return                        # client post-stop (finding 7)
            settings, units, health, log_text = self._fetch()
        if self.stop_event.is_set():          # finding 2
            return
        self.app.call_from_thread(self._apply, settings, units, health, log_text)

    def _report_error(self, exc: BaseException) -> None:
        self._drop_client()          # a broken client must never survive
                                      # into the next cycle
        if self.stop_event.is_set():          # finding 2
            return
        if isinstance(exc, SplitflapError):
            message = f"UNREACHABLE — {exc}"
        else:
            message = f"INTERNAL ERROR — {exc!r}"
        self.app.call_from_thread(self._apply_error, message)

    def _apply(self, settings: Settings, units: UnitsHealth,
               health: ClusterHealth, log_text: str | None) -> None:
        self.query_one("#detail-settings", Static).update(
            f"{self.board.name} [{settings.plat}] rev {settings.version} "
            f"mode={settings.device_mode or '-'} state={settings.cluster_state} "
            f"heap={settings.heap} rssi={settings.rssi} up={settings.up}s")
        faults = ", ".join(f"0x{u.address:02x}" for u in units.units if u.fault)
        self.query_one("#detail-units", Static).update(
            f"units {units.width} faulty {units.faulty}"
            + (f" [{faults}]" if faults else ""))
        extra = f" stackFree={health.stack_free}" if health.stack_free is not None else ""
        self.query_one("#detail-cluster", Static).update(
            f"cluster {health.state} hmac={'on' if health.hmac else 'off'} "
            f"foreign j/p/r {health.foreign_joins}/{health.foreign_pings}/"
            f"{health.foreign_renders}{extra}")
        if log_text:
            # Static has no readable `.renderable` accessor to append onto
            # (Textual 8.x) — keep our own running buffer instead, capped
            # (#452) so a long-open screen doesn't grow this string forever.
            self._log_text = cap_log(self._log_text + log_text)
            self.query_one("#detail-log", Static).update(self._log_text)

    def _apply_error(self, message: str) -> None:
        self.query_one("#detail-settings", Static).update(
            f"{self.board.name}: {message}")
