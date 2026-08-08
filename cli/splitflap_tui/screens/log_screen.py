from __future__ import annotations

from typing import Callable

from textual.app import ComposeResult
from textual.screen import Screen
from textual.widgets import Footer, Header, RichLog

from splitflap_client.logs import fetch_flash_log
from splitflap_client.transport import BoardClient, SplitflapError


class LogScreen(Screen):
    """Full-height flash-log viewer for the leader (S3-only route). `p`
    toggles between the current and previous boot's log (`?prev=1`) and
    re-fetches; each fetch is a one-shot thread worker, not a poll loop —
    the operator asks for a fresh read, the screen doesn't auto-refresh."""

    BINDINGS = [("escape", "app.pop_screen", "back"),
                ("p", "toggle_prev", "prev boot")]

    def __init__(self, url: str, client_factory: Callable[[str], BoardClient]):
        super().__init__()
        self.url = url
        self.factory = client_factory
        self.prev = False
        # #441 finding 6: consecutive `p` toggles spawn overlapping fetch
        # workers with no ordering guarantee — a slow first fetch resolving
        # AFTER a fast second one would overwrite the correct boot's log
        # with the stale one ("last-write-wins" picking the wrong writer).
        # Each _refresh() call claims the next generation; _apply/_apply_
        # error drop any result whose generation isn't the current one.
        self._gen = 0

    def compose(self) -> ComposeResult:
        yield Header()
        yield RichLog(id="flash-log", highlight=False, markup=False, wrap=False)
        yield Footer()

    def on_mount(self) -> None:
        self._refresh()

    def action_toggle_prev(self) -> None:
        self.prev = not self.prev
        self._refresh()

    def _refresh(self) -> None:
        self._gen += 1
        gen = self._gen
        prev = self.prev

        def work() -> None:
            try:
                with self.factory(self.url) as c:
                    text = fetch_flash_log(c, prev=prev)
                self.app.call_from_thread(self._apply, text, prev, gen)
            except SplitflapError as exc:
                self.app.call_from_thread(self._apply_error, str(exc), gen)
        self.run_worker(work, thread=True)

    def _apply(self, text: str, prev: bool, gen: int) -> None:
        if gen != self._gen:
            return                       # superseded by a later toggle
        log = self.query_one("#flash-log", RichLog)
        log.clear()
        log.write(f"-- {'previous' if prev else 'current'} boot --")
        for line in text.splitlines():
            log.write(line)

    def _apply_error(self, message: str, gen: int) -> None:
        if gen != self._gen:
            return                       # superseded by a later toggle
        log = self.query_one("#flash-log", RichLog)
        log.clear()
        log.write(f"UNREACHABLE — {message}")
