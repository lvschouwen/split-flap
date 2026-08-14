from __future__ import annotations

from textual.app import ComposeResult
from textual.containers import VerticalScroll
from textual.screen import Screen
from textual.widgets import Footer, Header, Static

from splitflap_client.models import UnitEntry

from ..health import unit_detail_sections

LABEL_WIDTH = 20

# Ops that need no argument run straight through the normal tiered dispatch,
# so they still get their confirm modal and their capability check.
DIRECT_OPS = {"h": "home", "i": "identify", "t": "self-test",
              "z": "reset-odometer"}

# Ops that take a value can't be a single keystroke — they pre-fill the
# command bar instead, leaving the operator to type the number and confirm.
PREFILL_OPS = {"o": "op offset", "j": "op jog", "g": "gates"}


class UnitDetailScreen(Screen):
    """One unit, everything it reports (#473) — the ~37-key ext-diag surface
    the dashboard table has room for eight of, plus its actions.

    Actions pop back to the dashboard before dispatching: the confirm modal
    and the result line live there, so running an op from here lands in the
    same place as running it from the command bar, rather than a second
    status surface that could disagree."""

    BINDINGS = [("escape", "app.pop_screen", "back")] + [
        (key, f"op_{key}", name) for key, name in DIRECT_OPS.items()
    ] + [(key, f"op_{key}", name) for key, name in PREFILL_OPS.items()]

    def __init__(self, entry: UnitEntry, row_median: int):
        super().__init__()
        self.entry = entry
        self.row_median = row_median

    def compose(self) -> ComposeResult:
        yield Header()
        with VerticalScroll():
            # markup=False: rev and flag strings come off the board.
            yield Static(id="unit-body", markup=False)
        yield Footer()

    def on_mount(self) -> None:
        self.sub_title = f"unit 0x{self.entry.address:02x}"
        lines: list[str] = []
        for title, rows in unit_detail_sections(self.entry, self.row_median):
            lines.append(f"── {title} ")
            for label, value in rows:
                lines.append(f"  {label:<{LABEL_WIDTH}}{value}")
            lines.append("")
        lines.append("  h home   i identify   t self-test   z reset-odometer")
        lines.append("  o offset   j jog   g gates   (these pre-fill the command bar)")
        self.query_one("#unit-body", Static).update("\n".join(lines))

    # One handler per bound key; Textual resolves action_op_<key>.
    def _direct(self, op: str) -> None:
        from ..commands import parse
        app = self.app
        app.pop_screen()
        app.dispatch_command(parse(f"op {op} {self.entry.address}"))

    def _prefill(self, prefix: str) -> None:
        app = self.app
        app.pop_screen()
        app.open_command_with(f"{prefix} {self.entry.address} ")

    def action_op_h(self) -> None: self._direct(DIRECT_OPS["h"])
    def action_op_i(self) -> None: self._direct(DIRECT_OPS["i"])
    def action_op_t(self) -> None: self._direct(DIRECT_OPS["t"])
    def action_op_z(self) -> None: self._direct(DIRECT_OPS["z"])
    def action_op_o(self) -> None: self._prefill(PREFILL_OPS["o"])
    def action_op_j(self) -> None: self._prefill(PREFILL_OPS["j"])
    def action_op_g(self) -> None: self._prefill(PREFILL_OPS["g"])
