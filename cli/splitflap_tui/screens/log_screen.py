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
        prev = self.prev

        def work() -> None:
            try:
                with self.factory(self.url) as c:
                    text = fetch_flash_log(c, prev=prev)
                self.app.call_from_thread(self._apply, text, prev)
            except SplitflapError as exc:
                self.app.call_from_thread(self._apply_error, str(exc))
        self.run_worker(work, thread=True)

    def _apply(self, text: str, prev: bool) -> None:
        log = self.query_one("#flash-log", RichLog)
        log.clear()
        log.write(f"-- {'previous' if prev else 'current'} boot --")
        for line in text.splitlines():
            log.write(line)

    def _apply_error(self, message: str) -> None:
        log = self.query_one("#flash-log", RichLog)
        log.clear()
        log.write(f"UNREACHABLE — {message}")
