from __future__ import annotations

from typing import Callable

from textual.app import App, ComposeResult
from textual.containers import Horizontal, Vertical
from textual.widgets import Footer, Header

from splitflap_client.events import DisplayEvent
from splitflap_client.models import ClusterStatus, StatusAggregate
from splitflap_client.transport import BoardClient

from .config import Config
from .poller import Poller
from .widgets import ClusterStrip, LogTail, StatsBar, UnitsTable, WallPanel


class SplitflapApp(App):
    TITLE = "splitflap"
    BINDINGS = [("q", "quit", "Quit")]

    def __init__(self, config: Config,
                 client_factory: Callable[[str], BoardClient] = BoardClient):
        super().__init__()
        self.config = config
        self.client_factory = client_factory
        self.connected = False
        self.wall_stale = True
        self.poller: Poller | None = None

    def compose(self) -> ComposeResult:
        yield Header()
        with Vertical():
            yield WallPanel(id="wall")
            yield ClusterStrip(id="cluster-strip")
            with Horizontal():
                yield UnitsTable(id="units")
                yield LogTail(id="log")
            yield StatsBar(id="stats")
        yield Footer()

    def on_mount(self) -> None:
        url = self.config.board_url()
        if not url:
            self.query_one("#stats", StatsBar).update(
                "no config — create ~/.config/splitflap/config.toml")
            return
        self.poller = Poller(self, self.client_factory, url,
                             self.config.poll_s, self.config.log_poll_s)
        self.poller.start()

    # ---- called from poller threads via call_from_thread ----
    def apply_status(self, agg: StatusAggregate, cluster: ClusterStatus) -> None:
        self.connected = True
        self.query_one("#cluster-strip", ClusterStrip).update_cluster(cluster)
        self.query_one("#units", UnitsTable).update_units(agg.units)
        self.query_one("#stats", StatsBar).update_stats(agg.stats_now, True)

    def apply_disconnect(self, message: str) -> None:
        self.connected = False
        stats = self.query_one("#stats", StatsBar)
        stats.update(f"DISCONNECTED — {message} (retrying)")

    def apply_log(self, lines: list[str]) -> None:
        log = self.query_one("#log", LogTail)
        log.clear()
        for line in lines:
            log.write(line)

    def apply_display(self, event: DisplayEvent) -> None:
        self.wall_stale = False
        self.query_one("#wall", WallPanel).update_wall(
            event.rows, event.text, stale=False)

    def apply_wall_stale(self) -> None:
        self.wall_stale = True
        wall = self.query_one("#wall", WallPanel)
        wall.border_title = "wall [STALE]"

    def on_unmount(self) -> None:
        if self.poller:
            self.poller.stop()
