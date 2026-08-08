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


class BoardDetailScreen(Screen):
    BINDINGS = [("escape", "app.pop_screen", "back")]

    def __init__(self, board: Board, client_factory: Callable[[str], BoardClient]):
        super().__init__()
        self.board = board
        self.factory = client_factory
        self.stop_event = threading.Event()
        self.log_cursor = 0
        self._log_text = ""

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

    def _poll(self) -> None:
        # #441 finding 1b: runs through the shared skeleton so a rendering
        # bug in _apply (reached via call_from_thread) can't silently kill
        # this thread the way the flood in #436 did to poller.py's loops.
        run_poll_loop(self.stop_event, POLL_S, self._cycle, self._report_error)

    def _cycle(self) -> None:
        with self.factory(self.board.url) as c:
            settings = Settings.from_json(c.get_json("/settings"))
            units = UnitsHealth.from_json(c.get_json("/units/health"))
            health = ClusterHealth.from_json(c.get_json("/cluster/health"))
            log_text = None
            if settings.plat == PLAT_ESP01:
                delta = fetch_follower_log(c, after=self.log_cursor)
                self.log_cursor = delta.cursor
                log_text = delta.text
        if self.stop_event.is_set():          # finding 2
            return
        self.app.call_from_thread(self._apply, settings, units, health, log_text)

    def _report_error(self, exc: BaseException) -> None:
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
            # (Textual 8.x) — keep our own running buffer instead.
            self._log_text += log_text
            self.query_one("#detail-log", Static).update(self._log_text)

    def _apply_error(self, message: str) -> None:
        self.query_one("#detail-settings", Static).update(
            f"{self.board.name}: {message}")
