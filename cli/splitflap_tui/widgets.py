from __future__ import annotations

from textual import events
from textual.widgets import DataTable, Input, RichLog, Static

from splitflap_client.models import ClusterStatus, SystemStatsNow, UnitsHealth


class CommandInput(Input):
    """Command-bar Input with k9s-style in-memory history recall (#446 fix
    round 1, item 3): up/down cycle previously submitted lines while this
    widget is focused. Implemented via on_key (not BINDINGS) deliberately —
    Input inherits ScrollableContainer's "up"/"down" -> scroll_up/scroll_down
    bindings, and those are only resolved once the raw Key message bubbles
    all the way up to the App; stopping the message here, before it bubbles,
    reliably suppresses them without fighting Textual's binding-merge order."""

    def __init__(self, **kw):
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

    def on_key(self, event: events.Key) -> None:
        if event.key == "up":
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
    def update_wall(self, rows: list[str] | None, text: str, stale: bool) -> None:
        body = "\n".join(rows) if rows else text
        self.border_title = "wall [STALE]" if stale else "wall"
        self.update(body or "(no display data)")


class ClusterStrip(Static):
    _text: str = ""

    def render_str(self) -> str:
        return self._text

    def update_cluster(self, c: ClusterStatus) -> None:
        lines = []
        for m in c.members:
            flags = [f for f, on in (("SUSPECT", m.suspect),
                                     ("DEGRADED", m.degraded),
                                     ("RESCUE", m.rescue),
                                     ("UPD-BLOCKED", m.update_blocked),
                                     ("STUCK", m.render_stuck)) if on]
            who = "self" if m.self_row else m.host
            state = "joined" if m.joined else f"lost({m.failures})"
            lines.append(f"row{m.row} {who} [{m.plat}] {m.rev} {state} "
                         + (" ".join(flags) if flags else "ok"))
        if c.rollout_phase and c.rollout_phase != "idle":
            lines.append(f"rollout: {c.rollout_phase} src={c.rollout_src or 's3'}")
        self._text = "\n".join(lines) or "(cluster disabled)"
        self.update(self._text)


class UnitsTable(DataTable):
    COLUMNS = ("addr", "st", "sx", "odo", "vmin", "gates", "flags")

    def on_mount(self) -> None:
        self.add_columns(*self.COLUMNS)

    def update_units(self, h: UnitsHealth) -> None:
        def cell(v):
            return "—" if v is None else str(v)
        self.clear()
        for u in h.units:
            flags = "STALE" if u.stale else ("FAULT" if u.fault else "")
            self.add_row(f"0x{u.address:02x}", str(u.state), cell(u.sx),
                         cell(u.odo), cell(u.vcc_min), cell(u.gates), flags)


class LogTail(RichLog):
    pass


class StatsBar(Static):
    def update_stats(self, s: SystemStatsNow, connected: bool) -> None:
        link = "connected" if connected else "DISCONNECTED — retrying"
        self.update(f"{link} | heap {s.heap} (min {s.min_heap}) | "
                    f"rssi {s.rssi} | up {s.uptime}s | "
                    f"i2c {s.i2c_tx}/{s.i2c_err} err")
