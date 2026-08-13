from __future__ import annotations

from rich.text import Text
from textual.app import ComposeResult
from textual.screen import ModalScreen
from textual.widgets import DataTable, Label

from ..commands import HELP


class HelpScreen(ModalScreen[None]):
    """Command reference (#451) — content generated from commands.HELP so it
    cannot drift from the parser (drift-gated in test_commands)."""

    BINDINGS = [("escape", "close", "close"), ("q", "close", "close")]

    def compose(self) -> ComposeResult:
        yield Label("commands — esc/q to close")
        yield DataTable(id="help-table")

    def on_mount(self) -> None:
        table = self.query_one("#help-table", DataTable)
        table.add_columns("command", "tier", "what")
        for e in HELP:
            # DataTable's default cell formatter runs plain str cells through
            # Text.from_markup — "reboot [board]" would render as "reboot "
            # (the unclosed tag swallows the rest). Text renders literally.
            table.add_row(Text(e.usage), Text(e.tier), Text(e.blurb))

    def action_close(self) -> None:
        self.dismiss(None)
