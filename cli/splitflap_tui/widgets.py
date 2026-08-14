from __future__ import annotations

from rich.text import Text
from textual import events
from textual.suggester import SuggestFromList
from textual.widgets import DataTable, Input, RichLog, Static

from splitflap_client.models import (ClusterMember, ClusterStatus,
                                      SystemStatsNow, UnitsHealth)

from .flapwall import wall_cells
from .format import human_duration, human_size
from .wear import row_sxl_median, unit_markers


SUGGEST_WORDS = ["stop", "text", "mode", "notify", "set", "op", "gates",
                 "reboot", "reset-units", "addr", "promote",
                 "cluster leave", "cluster config", "help"]


def border_text(s: str) -> Text:
    """Textual's border_title setter unconditionally parses a plain str as
    Rich console markup (Widget.render_str -> Content.from_markup),
    regardless of a widget's own markup=False — a bracketed literal like
    "cluster [STALE]" silently loses everything from "[STALE]" onward (the
    unclosed tag swallows the rest). Wrapping in rich.text.Text takes the
    special-cased from_rich_text path instead and renders literally.

    Public (no leading underscore): every border_title assignment in this
    module goes through it, AND app.py's apply_wall_stale() — the one
    border_title assignment outside this module — must too (#441 follow-up:
    it was still a raw f-string, so WallPanel's "STALE" suffix was silently
    swallowed the same way "cluster [STALE]" was before this helper
    existed)."""
    return Text(s)


def format_cmd_status(text: str, duration_s: float | None, clock: str) -> Text:
    """Status-line Text (#451): glyph + timestamp + literal result body.
    The ⛔-prefix convention (execute() puts it on every error) drives the
    glyph and is stripped from the body so it doesn't render twice."""
    ok = not text.startswith("⛔")
    body = text[2:] if text.startswith("⛔ ") else text
    out = Text()
    out.append("✓" if ok else "⛔", style="#FFB000" if ok else "#E05B4B")
    out.append(f" {clock}  ", style="dim")
    out.append(body)                                  # literal, never markup
    if duration_s is not None:
        out.append(f" ({duration_s:.1f} s)", style="dim")
    return out


DOT_OK = "#FFB000"
DOT_WARN = "#D9A03F"
DOT_BAD = "#E05B4B"


def member_dot_style(m: ClusterMember) -> str:
    if not m.joined or m.degraded or m.rescue or m.render_stuck:
        return DOT_BAD
    if m.suspect or m.update_blocked:
        return DOT_WARN
    return DOT_OK


class CommandInput(Input):
    """Command-bar Input with k9s-style in-memory history recall (#446 fix
    round 1, item 3): up/down cycle previously submitted lines while this
    widget is focused. Implemented via on_key (not BINDINGS) deliberately —
    Input inherits ScrollableContainer's "up"/"down" -> scroll_up/scroll_down
    bindings, and those are only resolved once the raw Key message bubbles
    all the way up to the App; stopping the message here, before it bubbles,
    reliably suppresses them without fighting Textual's binding-merge order."""

    def __init__(self, **kw):
        kw.setdefault("suggester",
                      SuggestFromList(SUGGEST_WORDS, case_sensitive=True))
        super().__init__(**kw)
        self.history: list[str] = []
        self._history_index = 0

    def remember(self, line: str) -> None:
        line = line.strip()
        if line and (not self.history or self.history[-1] != line):
            self.history.append(line)
        self._history_index = len(self.history)

    def reset_history_cursor(self) -> None:
        self._history_index = len(self.history)

    def dismiss_bar(self) -> None:
        """Abandon whatever is typed and put the bar away (#454). Submit and
        escape MUST leave identical state; the two paths having separate
        implementations is what left escape a no-op, and a bar that stays
        focused swallows every dashboard hotkey — including quit — as text."""
        self.value = ""
        self.display = False
        self.reset_history_cursor()
        self.app.set_focus(None)

    def on_key(self, event: events.Key) -> None:
        if event.key == "escape":
            event.stop()
            event.prevent_default()
            self.dismiss_bar()
        elif event.key == "up":
            event.stop()
            event.prevent_default()
            if self.history:
                self._history_index = max(0, self._history_index - 1)
                self.value = self.history[self._history_index]
                self.cursor_position = len(self.value)
        elif event.key == "down":
            event.stop()
            event.prevent_default()
            if self._history_index < len(self.history):
                self._history_index += 1
            self.value = (self.history[self._history_index]
                         if self._history_index < len(self.history) else "")
            self.cursor_position = len(self.value)


class WallPanel(Static):
    """markup=False (#441 finding 1a): the wall renders board/firmware
    -supplied display text verbatim — a payload like "[/]" is valid content,
    not Rich console markup, and must never be parsed as such. #450: body is
    flap cells (flapwall.wall_cells) when the panel is wide enough, plain
    text otherwise; on_resize re-decides."""

    def __init__(self, **kw):
        kw.setdefault("markup", False)
        super().__init__(**kw)
        self._rows: list[str] | None = None
        self._text = ""

    def wall_text(self) -> str:
        return "\n".join(self._rows) if self._rows else self._text

    def update_wall(self, rows: list[str] | None, text: str, stale: bool) -> None:
        self._rows, self._text = rows, text
        self.border_title = border_text("wall [STALE]" if stale else "wall")
        self._refresh_body()

    def _refresh_body(self) -> None:
        body = self.wall_text()
        if not body:
            self.update("(no display data)")
            return
        # Pre-layout content_size is 0x0 — assume wide, on_resize corrects.
        width = self.content_size.width or 200
        cells = wall_cells(self._rows, self._text, width)
        self.update(cells if cells is not None else body)

    def on_resize(self, event) -> None:
        self._refresh_body()


class ClusterStrip(Static):
    """markup=False (#441 finding 1a): member host/rev strings are
    board-supplied. Finding 5: border_title carries a " [STALE]" suffix
    while the leader is unreachable, cleared on the next successful poll."""

    BASE_TITLE = "cluster"
    _text: str = ""

    def __init__(self, **kw):
        kw.setdefault("markup", False)
        super().__init__(**kw)

    def on_mount(self) -> None:
        self.border_title = border_text(self.BASE_TITLE)

    def cluster_text(self) -> str:
        # NOT named render_str: that name collides with Widget.render_str
        # (str | Content -> Content, used internally by the border_title
        # setter) — the collision was latent until border_title was ever
        # assigned on this widget (finding 5's staleness marker).
        return self._text

    def update_cluster(self, c: ClusterStatus) -> None:
        out = Text()
        plain_lines = []
        for m in c.members:
            flags = [f for f, on in (("SUSPECT", m.suspect),
                                     ("DEGRADED", m.degraded),
                                     ("RESCUE", m.rescue),
                                     ("UPD-BLOCKED", m.update_blocked),
                                     ("STUCK", m.render_stuck)) if on]
            who = "self" if m.self_row else m.host
            state = "joined" if m.joined else f"lost({m.failures})"
            line = (f"row{m.row}  {who:<16.16} {m.plat:<8} {m.rev:<10.10} "
                    f"{state}" + (" " + " ".join(flags) if flags else ""))
            if plain_lines:
                out.append("\n")
            out.append("● ", style=member_dot_style(m))
            out.append(line)                       # literal, never markup
            plain_lines.append("● " + line)
        if c.rollout_phase and c.rollout_phase != "idle":
            line = f"rollout: {c.rollout_phase} src={c.rollout_src or 's3'}"
            if plain_lines:
                out.append("\n")
            out.append(line)
            plain_lines.append(line)
        self._text = "\n".join(plain_lines) or "(cluster disabled)"
        self.update(out if plain_lines else "(cluster disabled)")

    def mark_stale(self) -> None:
        self.border_title = border_text(f"{self.BASE_TITLE} [STALE]")
        self.add_class("stale")

    def clear_stale(self) -> None:
        self.border_title = border_text(self.BASE_TITLE)
        self.remove_class("stale")


def units_rows(h: UnitsHealth) -> list[tuple[str, ...]]:
    """Pure row builder for UnitsTable (#455) — one tuple per unit, in
    COLUMNS order. Kept out of the widget so the wear markers can be tested
    against real board payloads without standing up an app."""
    def cell(v):
        return "—" if v is None else str(v)
    median = row_sxl_median(h.units)
    return [(f"0x{u.address:02x}", str(u.state), cell(u.sxl),
             cell(u.hall_fails), cell(u.odo), cell(u.vcc_min), cell(u.gates),
             " ".join(unit_markers(u, median)))
            for u in h.units]


class UnitsTable(DataTable):
    """Finding 5: same border-staleness pattern as ClusterStrip/LogTail.

    #455: the wear columns are LIFETIME (`sxl`, `hf`), not since-boot — `sx`
    read 0 on every unit of a 24 h-old wall that had two units needing
    physical attention, so it was costing a column and showing nothing."""

    BASE_TITLE = "units"
    COLUMNS = ("addr", "st", "sxl", "hf", "odo", "vmin", "gates", "flags")

    def on_mount(self) -> None:
        self.add_columns(*self.COLUMNS)
        self.border_title = border_text(self.BASE_TITLE)

    def update_units(self, h: UnitsHealth) -> None:
        self.clear()
        for row in units_rows(h):
            # Text() per #450: DataTable markup-parses plain-str cells.
            self.add_row(*(Text(c) for c in row))

    def mark_stale(self) -> None:
        self.border_title = border_text(f"{self.BASE_TITLE} [STALE]")
        self.add_class("stale")

    def clear_stale(self) -> None:
        self.border_title = border_text(self.BASE_TITLE)
        self.remove_class("stale")


class LogTail(RichLog):
    """markup=False (#441 finding 1a — RichLog already defaults to False,
    made explicit here so it can never silently flip): the dashboard log
    tail writes board flash-log lines verbatim. Finding 5: same
    border-staleness pattern as ClusterStrip/UnitsTable."""

    BASE_TITLE = "log"

    def __init__(self, **kw):
        kw.setdefault("markup", False)
        super().__init__(**kw)

    def on_mount(self) -> None:
        self.border_title = border_text(self.BASE_TITLE)

    def mark_stale(self) -> None:
        self.border_title = border_text(f"{self.BASE_TITLE} [STALE]")
        self.add_class("stale")

    def clear_stale(self) -> None:
        self.border_title = border_text(self.BASE_TITLE)
        self.remove_class("stale")


class StatsBar(Static):
    """markup=False (#441 finding 1a): stats text is board/firmware-numeric
    but the DISCONNECTED path folds in the raw SplitflapError message,
    which can itself carry a board-supplied HTTP error body."""

    def __init__(self, **kw):
        kw.setdefault("markup", False)
        super().__init__(**kw)

    def update_stats(self, s: SystemStatsNow, connected: bool) -> None:
        link = "connected" if connected else "DISCONNECTED — retrying"
        self.update(f"{link} · heap {human_size(s.heap)} "
                    f"(min {human_size(s.min_heap)}) · rssi {s.rssi} · "
                    f"up {human_duration(s.uptime)} · "
                    f"i2c {s.i2c_tx}/{s.i2c_err} err")
