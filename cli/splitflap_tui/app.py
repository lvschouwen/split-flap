from __future__ import annotations

from typing import Callable

from textual.app import App, ComposeResult
from textual.binding import Binding
from textual.containers import Horizontal, Vertical
from textual.widgets import Footer, Header, Input, Static

from splitflap_client import control
from splitflap_client.capability import CLIENT_ROUTES, PLAT_S3, serves
from splitflap_client.events import DisplayEvent
from splitflap_client.models import ClusterStatus, StatusAggregate
from splitflap_client.ops import OpResult, run_op
from splitflap_client.transport import BoardClient, HttpError, SplitflapError

from .commands import (CommandError, ParsedCommand, TIER_KILL, TIER_ROUTINE,
                       TIER_TYPED, parse)
from .config import Config
from .confirm import ConfirmModal
from .poller import Poller
from .widgets import (ClusterStrip, CommandInput, LogTail, StatsBar,
                      UnitsTable, WallPanel)


def _route_known_anywhere(route: tuple[str, str]) -> bool:
    method, path = route
    return any((method, path) in routes for routes in CLIENT_ROUTES.values())


def _format_op_result(r: OpResult) -> str:
    parts = [r.state]
    if r.reason:
        parts.append(r.reason)
    if r.detail:
        parts.append(r.detail)
    return ", ".join(parts)


def _execute_op(client: BoardClient, parsed: ParsedCommand) -> str:
    path = parsed.route[1]
    params: dict = {"address": parsed.args["unit"]}
    if path == "/unit/offset":
        params["value"] = parsed.args["value"]
    elif path == "/unit/jog":
        params["steps"] = parsed.args["value"]
    return _format_op_result(run_op(client, path, params))


def _execute_addr(client: BoardClient, parsed: ParsedCommand) -> str:
    unit = parsed.args["unit"]
    if parsed.args["mode"] == "burn":
        # VERIFIED (Task 7, WebMaintenance.cpp:305-334): /unit/set-address
        # takes query params "address" (source) + "value" (target) — NOT
        # "target". ParsedCommand.args keeps the readable key "target"; this
        # is the one place that maps it onto the wire's "value" param.
        client.post("/unit/set-address",
                    params={"address": unit, "value": parsed.args["target"]})
        return f"ok — address {unit} -> {parsed.args['target']}"
    client.post("/unit/clear-address", params={"address": unit})
    return f"ok — address {unit} cleared"


def execute(parsed: ParsedCommand, client: BoardClient) -> str:
    """Runs one parsed command against client; returns the status-line text.
    Every HttpError renders verbatim (⛔ status: body); OpResult renders
    as "state[, reason[, detail]]"."""
    try:
        if parsed.name == "stop":
            control.stop(client)
            return "stopped"
        if parsed.name == "text":
            control.set_text(client, parsed.args["text"])
            return f"ok — {parsed.summary}"
        if parsed.name == "mode":
            control.set_mode(client, parsed.args["mode"])
            return f"ok — {parsed.summary}"
        if parsed.name == "notify":
            control.notify(client, parsed.args["text"], parsed.args["dwell"])
            return f"ok — {parsed.summary}"
        if parsed.name == "set":
            control.set_setting(client, parsed.args["field"], parsed.args["value"])
            return f"ok — {parsed.summary}"
        if parsed.name == "op":
            return _execute_op(client, parsed)
        if parsed.name == "gates":
            result = run_op(client, "/unit/gates", {
                "address": parsed.args["unit"], "gates": parsed.args["mask"]})
            return _format_op_result(result)
        if parsed.name == "reboot":
            control.reboot(client)
            return "rebooting"
        if parsed.name == "addr":
            return _execute_addr(client, parsed)
        if parsed.name == "config":
            # VERIFIED (WebCluster.cpp:483-491): "members" is a POST FORM
            # param (hasParam(..., true)), not a query param — use data=.
            client.post("/cluster/config", data={"members": parsed.args["members"]})
            return f"ok — {parsed.summary}"
        # /cluster/leave is documented on NO platform's capability table —
        # the firmware serves it but its own /api index omits it (#448) —
        # so it isn't gated client-side either; it and reset-units/promote
        # just pass straight through to the wire.
        client.post(parsed.route[1])
        return f"ok — {parsed.summary}"
    except HttpError as exc:
        return f"⛔ {exc.status}: {exc.body}"
    except SplitflapError as exc:
        return f"⛔ {exc}"


class SplitflapApp(App):
    TITLE = "splitflap"
    BINDINGS = [("q", "quit", "Quit"), (":", "open_command", "Command"),
                Binding("ctrl+s", "stop_wall", "STOP", priority=True)]

    def __init__(self, config: Config,
                 client_factory: Callable[[str], BoardClient] = BoardClient):
        super().__init__()
        self.config = config
        self.client_factory = client_factory
        self.connected = False
        self.wall_stale = True
        self.poller: Poller | None = None
        self.plat = PLAT_S3          # default before the first /status poll

    def compose(self) -> ComposeResult:
        yield Header()
        with Vertical():
            yield WallPanel(id="wall")
            yield ClusterStrip(id="cluster-strip")
            with Horizontal():
                yield UnitsTable(id="units")
                yield LogTail(id="log")
            yield StatsBar(id="stats")
            yield Static("press ':' for commands", id="cmd-status")
            command_input = CommandInput(id="command")
            command_input.display = False
            yield command_input
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
        self.plat = agg.settings.plat
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

    # ---- command bar ----
    def action_open_command(self) -> None:
        cmd = self.query_one("#command", CommandInput)
        cmd.reset_history_cursor()
        cmd.display = True
        cmd.focus()

    def action_stop_wall(self) -> None:
        """Dedicated always-active STOP key (ctrl+s, priority binding — see
        BINDINGS) — same tiered dispatch as the ":stop" command; stop is
        TIER_KILL so this still runs with no confirm, but still gets the
        capability check. Priority=True means this fires even while the
        command Input or a confirm modal has focus."""
        self.dispatch_command(parse("stop"))

    def on_input_submitted(self, event: Input.Submitted) -> None:
        if event.input.id != "command":
            return
        line = event.value
        event.input.remember(line)
        event.input.value = ""
        event.input.display = False
        self.set_focus(None)
        try:
            parsed = parse(line)
        except CommandError as exc:
            self.apply_cmd_result(f"⛔ {exc}")
            return
        self.dispatch_command(parsed)

    def _capability_gate(self, parsed: ParsedCommand) -> bool:
        """Client-side capability gate, run before any confirm modal or
        request goes out. A route documented on SOME platform's
        CLIENT_ROUTES but not on self.plat is rejected outright. A route
        documented on NO platform's table (currently only /cluster/leave —
        the firmware serves it, but its own /api index omits it; tracked
        as #448) is not ours to gate: unknown routes are the firmware's
        call, so they pass through to the wire untouched."""
        method, path = parsed.route
        if _route_known_anywhere(parsed.route) and not serves(self.plat, method, path):
            self.apply_cmd_result(f"⛔ {parsed.name}: not served on {self.plat}")
            return False
        return True

    def dispatch_command(self, parsed: ParsedCommand) -> None:
        if not self._capability_gate(parsed):
            return
        if parsed.tier in (TIER_KILL, TIER_ROUTINE):
            self.run_command(parsed)
            return
        typed = parsed.tier == TIER_TYPED
        token = str(parsed.args.get("unit", parsed.name)) if typed else ""

        def on_result(confirmed: bool) -> None:
            if confirmed:
                self.run_command(parsed)
            else:
                self.apply_cmd_result("cancelled")

        self.push_screen(ConfirmModal(parsed.summary, typed=typed, token=token),
                         on_result)

    def run_command(self, parsed: ParsedCommand) -> None:
        def work() -> None:
            try:
                with self.client_factory(self.config.board_url()) as client:
                    result = execute(parsed, client)
            except SplitflapError as exc:
                result = f"⛔ {exc}"
            self.call_from_thread(self.apply_cmd_result, result)
        self.run_worker(work, thread=True)

    def apply_cmd_result(self, text: str) -> None:
        self.query_one("#cmd-status", Static).update(text)

    def on_unmount(self) -> None:
        if self.poller:
            self.poller.stop()
