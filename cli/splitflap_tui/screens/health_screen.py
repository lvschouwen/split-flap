from __future__ import annotations

from textual.app import ComposeResult
from textual.containers import VerticalScroll
from textual.screen import Screen
from textual.widgets import Footer, Header, Static

from splitflap_client.models import StatusAggregate

from ..health import health_sections

LABEL_WIDTH = 16


class HealthScreen(Screen):
    """Board health (#472): the OTA/rescue/boot/task-stack/system block that
    the client already parsed but nothing rendered.

    Costs no extra request — it reads the SAME `/status` aggregate the
    dashboard poller is already fetching, and the app re-renders it here on
    every poll while this screen is open (the poller does not stop for a
    pushed screen), so an open health screen shows the newest reading rather
    than the one it was opened with."""

    BINDINGS = [("escape", "app.pop_screen", "back")]

    def compose(self) -> ComposeResult:
        yield Header()
        with VerticalScroll():
            # markup=False: values are board-supplied strings (reset reason,
            # last-invalid rev, flash result) and must render verbatim.
            yield Static(id="health-body", markup=False)
        yield Footer()

    def on_mount(self) -> None:
        self.render_status(getattr(self.app, "last_status", None))

    def render_status(self, agg: StatusAggregate | None) -> None:
        body = self.query_one("#health-body", Static)
        if agg is None:
            # Not the same as "all readings absent": a page of dashes would
            # look like data. Say there is none.
            body.update("no status yet — the leader has not answered a poll")
            return
        lines: list[str] = []
        for title, rows in health_sections(agg):
            lines.append(f"── {title} ")
            if not rows:
                lines.append(f"  {'(none reported)':<{LABEL_WIDTH}}")
            for label, value in rows:
                lines.append(f"  {label:<{LABEL_WIDTH}}{value}")
            lines.append("")
        body.update("\n".join(lines))
