from __future__ import annotations

from typing import Callable

from rich.text import Text
from textual.app import ComposeResult
from textual.screen import Screen
from textual.widgets import DataTable, Footer, Header, Static

from splitflap_client.discover import DiscoveredBoard, run_discover
from splitflap_client.transport import BoardClient, HttpError, SplitflapError

from ..widgets import border_text


class DiscoverScreen(Screen):
    """Result of a leader-side mDNS scan (#469): `POST /cluster/discover` arms
    the browse in netTask, then the GET is polled until it reports done. Runs
    on mount, `r` rescans, `escape` pops.

    Each scan is a one-shot thread worker, not a poll loop — the operator asks
    for a fresh scan, the screen doesn't auto-refresh (an mDNS browse is real
    work on the leader's netTask). Repeated `r` presses spawn overlapping
    workers with no ordering guarantee, so each claims a generation and a
    result whose generation has been superseded is dropped — the same hazard
    LogScreen's guard exists for."""

    BINDINGS = [("escape", "app.pop_screen", "back"),
                ("r", "rescan", "rescan")]

    def __init__(self, url: str, client_factory: Callable[[str], BoardClient]):
        super().__init__()
        self.url = url
        self.factory = client_factory
        self._gen = 0

    def compose(self) -> ComposeResult:
        yield Header()
        # markup=False: a board-supplied error body renders verbatim here.
        yield Static("scanning…", id="discover-status", markup=False)
        yield DataTable(id="discover-table")
        yield Footer()

    def on_mount(self) -> None:
        table = self.query_one("#discover-table", DataTable)
        table.add_columns("name", "host", "rev", "width", "plat")
        table.border_title = border_text("boards")   # #450 panel identity
        self._scan()

    def action_rescan(self) -> None:
        self._scan()

    def _scan(self) -> None:
        self._gen += 1
        gen = self._gen
        self.query_one("#discover-status", Static).update("scanning…")

        def work() -> None:
            try:
                with self.factory(self.url) as c:
                    boards = run_discover(c)
                self.app.call_from_thread(self._apply, boards, gen)
            except HttpError as exc:
                self.app.call_from_thread(self._apply_error,
                                          f"⛔ {exc.status}: {exc.body}", gen)
            except SplitflapError as exc:
                self.app.call_from_thread(self._apply_error, f"⛔ {exc}", gen)
        self.run_worker(work, thread=True)

    def _apply(self, boards: list[DiscoveredBoard], gen: int) -> None:
        if gen != self._gen:
            return                       # superseded by a later rescan
        table = self.query_one("#discover-table", DataTable)
        table.clear()
        for b in boards:
            # DataTable markup-parses plain str cells; a board-supplied name
            # carrying a "[" would swallow the rest of the row (same reason
            # HelpScreen wraps its cells).
            table.add_row(Text(b.name), Text(b.host), Text(b.rev),
                          Text(str(b.width)), Text(b.plat))
        # An empty scan is the normal answer across the operator VPN — mDNS
        # is link-local, so it only ever finds boards on the leader's own
        # subnet. Say so plainly rather than rendering it as a failure.
        summary = (f"{len(boards)} board(s) — r rescan · esc back" if boards
                   else "no boards found — r rescan · esc back")
        self.query_one("#discover-status", Static).update(summary)

    def _apply_error(self, message: str, gen: int) -> None:
        if gen != self._gen:
            return                       # superseded by a later rescan
        self.query_one("#discover-status", Static).update(message)
