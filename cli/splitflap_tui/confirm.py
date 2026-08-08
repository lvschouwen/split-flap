from __future__ import annotations

from textual.app import ComposeResult
from textual.containers import Vertical
from textual.screen import ModalScreen
from textual.widgets import Input, Label


class ConfirmModal(ModalScreen[bool]):
    BINDINGS = [("y", "yes", "confirm"), ("n", "no", "cancel"),
                ("escape", "no", "cancel")]

    def __init__(self, summary: str, typed: bool = False, token: str = ""):
        super().__init__()
        self.summary = summary
        self.typed = typed
        self.token = token

    def compose(self) -> ComposeResult:
        # markup=False (#441 finding 1a): summary/token can carry
        # board-supplied or user-typed text (e.g. "text [ICE 704]") that
        # must render literally, never be parsed as Rich console markup.
        with Vertical(id="confirm-box"):
            yield Label(self.summary, markup=False)
            if self.typed:
                yield Label(f"type '{self.token}' to confirm, Esc to cancel",
                           markup=False)
                yield Input(id="confirm-input")
            else:
                yield Label("y to confirm, n/Esc to cancel")

    def action_yes(self) -> None:
        if not self.typed:
            self.dismiss(True)

    def action_no(self) -> None:
        self.dismiss(False)

    def on_input_submitted(self, event: Input.Submitted) -> None:
        self.dismiss(event.value.strip() == self.token)
