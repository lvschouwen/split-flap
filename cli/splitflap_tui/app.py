from __future__ import annotations

import time
from datetime import datetime
from pathlib import Path
from typing import Callable

from textual.app import App, ComposeResult
from textual.binding import Binding
from textual.containers import Horizontal, Vertical
from textual.widgets import Footer, Header, Input, Static

from splitflap_client import control
from splitflap_client.capability import CLIENT_ROUTES, PLAT_S3, serves
from splitflap_client.events import DisplayEvent
from splitflap_client.models import ClusterStatus, StatusAggregate
from splitflap_client.ops import (OpResult, SelfTestResult,
                                  parse_self_test_result, run_op, submit_op,
                                  wait_op)
from splitflap_client.transport import BoardClient, HttpError, SplitflapError

from .commands import (CommandError, ParsedCommand, TIER_KILL, TIER_ROUTINE,
                       TIER_TYPED, parse)
from .config import Config
from .confirm import ConfirmModal
from .history import load_history, save_history
from .poller import Poller
from .screens.board_detail import BoardDetailScreen
from .screens.discover_screen import DiscoverScreen
from .screens.health_screen import HealthScreen
from .screens.help_screen import HelpScreen
from .screens.log_screen import LogScreen
from .widgets import (ClusterStrip, CommandInput, LogTail, StatsBar,
                      UnitsTable, WallPanel, border_text, format_cmd_status)


def _route_known_anywhere(route: tuple[str, str]) -> bool:
    method, path = route
    return any((method, path) in routes for routes in CLIENT_ROUTES.values())


DISPLAY_NAMES = {"config": "cluster config", "cluster-leave": "cluster leave"}


def _format_op_result(r: OpResult) -> str:
    parts = [r.state]
    if r.reason:
        parts.append(r.reason)
    if r.detail:
        parts.append(r.detail)
    return ", ".join(parts)


def _format_self_test_result(r: SelfTestResult) -> str:
    parts = [r.state, f"steps_per_rev={r.steps_per_rev}",
             f"hall_window={r.hall_window}", f"rev_time_ms={r.rev_time_ms}"]
    if r.state != "ok":
        if r.reason:
            parts.append(f"reason={r.reason}")
        if r.unit_reason:
            parts.append(f"unit_reason={r.unit_reason}")
    return ", ".join(parts)


def _execute_op(client: BoardClient, parsed: ParsedCommand) -> str:
    path = parsed.route[1]
    params: dict = {"address": parsed.args["unit"]}
    if path == "/unit/offset":
        params["value"] = parsed.args["value"]
    elif path == "/unit/jog":
        params["steps"] = parsed.args["value"]
    if path == "/unit/self-test":
        # #441 finding 3: /unit/op-result's "ok" doesn't carry the
        # measurements, and on the follower means only "started" — false
        # success is possible there. Self-test has its own result contract:
        # poll /unit/self-test-result and parse it with SelfTestResult.
        seq = submit_op(client, path, params)
        result = wait_op(client, seq, result_path="/unit/self-test-result",
                         parse=parse_self_test_result)
        return _format_self_test_result(result)
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


def execute(parsed: ParsedCommand, client: BoardClient, config: Config,
           client_factory: Callable[[str], BoardClient]) -> str:
    """Runs one parsed command against client; returns the status-line text.
    Every HttpError renders verbatim (⛔ status: body); OpResult renders
    as "state[, reason[, detail]]".

    client is already opened against config.board_url() (the default
    board) — every command uses it EXCEPT reboot, whose spec-mandated
    dangerous-tier form is `reboot [board]` (#441 finding 4): it resolves
    its own target via config/client_factory and ignores the passed-in
    client entirely when a board name is given."""
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
            board = parsed.args.get("board", "")
            url = config.board_url(board)
            if not url:
                return f"⛔ unknown board: {board}"
            with client_factory(url) as reboot_client:
                control.reboot(reboot_client)
            return f"rebooting {board}" if board else "rebooting"
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
    CSS_PATH = "splitflap.tcss"
    BINDINGS = [("q", "quit", "Quit"), (":", "open_command", "Command"),
                Binding("ctrl+s", "stop_wall", "STOP", priority=True),
                ("b", "board_detail", "Board"), ("l", "log_screen", "Log"),
                ("s", "health_screen", "Health"),
                ("question_mark", "help", "Help")]

    def __init__(self, config: Config,
                 client_factory: Callable[[str], BoardClient] = BoardClient,
                 history_path: Path | None = None):
        super().__init__()
        self.config = config
        self.client_factory = client_factory
        self.history_path = history_path
        self.connected = False
        self.wall_stale = True
        self.poller: Poller | None = None
        self.plat = PLAT_S3          # default before the first /status poll
        self.device_mode = ""        # "" until the first successful poll
        self._board_cycle_index = 0
        # Newest /status aggregate, kept so HealthScreen (#472) can render
        # without issuing a request of its own. None until the first
        # successful poll — that is NOT the same as all-values-absent.
        self.last_status: StatusAggregate | None = None
        self._last_cmd_result = ""

    def compose(self) -> ComposeResult:
        yield Header()
        with Vertical():
            yield WallPanel(id="wall")
            yield ClusterStrip(id="cluster-strip")
            with Horizontal(id="main-split"):
                yield UnitsTable(id="units")
                yield LogTail(id="log")
            yield StatsBar(id="stats")
            # markup=False (#441 finding 1a): op/HttpError results folded in
            # here can carry a board-supplied error body verbatim.
            yield Static("press ':' for commands", id="cmd-status", markup=False)
            command_input = CommandInput(id="command")
            command_input.display = False
            yield command_input
        yield Footer()

    def on_mount(self) -> None:
        cmd = self.query_one("#command", CommandInput)
        cmd.history = load_history(self.history_path)
        cmd.reset_history_cursor()
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
        self.last_status = agg
        if isinstance(self.screen, HealthScreen):
            self.screen.render_status(agg)   # keep an open screen live
        self.plat = agg.settings.plat
        self.device_mode = agg.settings.device_mode
        s = agg.settings
        role = "leading" if s.cluster_leading else (s.cluster_state or "standalone")
        # sub_title carries board-supplied strings; safe because
        # App.format_title builds Content(title) literally (verified Textual
        # 8.2.8) — if a Textual upgrade ever markup-parses titles, wrap
        # these values.
        self.sub_title = (f"{s.effective_device_name or 'wall'} · "
                          f"{s.device_mode or '-'} · {role} · rev {s.version}")
        cluster_strip = self.query_one("#cluster-strip", ClusterStrip)
        cluster_strip.update_cluster(cluster)
        cluster_strip.clear_stale()
        units = self.query_one("#units", UnitsTable)
        units.update_units(agg.units)
        units.clear_stale()
        self.query_one("#log", LogTail).clear_stale()
        self.query_one("#stats", StatsBar).update_stats(agg.stats_now, True)

    def apply_disconnect(self, message: str) -> None:
        # #441 finding 5: the stats bar isn't the only stale panel while
        # the leader is unreachable — the cluster strip, units table and
        # log tail last showed data from before the disconnect too.
        self.connected = False
        stats = self.query_one("#stats", StatsBar)
        stats.update(f"DISCONNECTED — {message} (retrying)")
        self.query_one("#cluster-strip", ClusterStrip).mark_stale()
        self.query_one("#units", UnitsTable).mark_stale()
        self.query_one("#log", LogTail).mark_stale()

    def apply_log(self, lines: list[str]) -> None:
        log = self.query_one("#log", LogTail)
        log.clear()
        for line in lines:
            log.write(line)

    def apply_display(self, event: DisplayEvent) -> None:
        self.wall_stale = False
        wall = self.query_one("#wall", WallPanel)
        wall.update_wall(event.rows, event.text, stale=False)
        wall.remove_class("stale")

    def apply_wall_stale(self) -> None:
        # border_text (#441 follow-up): a raw "wall [STALE]" string here
        # gets markup-parsed by Textual's border_title setter regardless of
        # WallPanel's own markup=False — the unclosed "[STALE]" tag
        # silently swallows itself, same bug widgets.py's border_title
        # assignments were fixed for.
        self.wall_stale = True
        wall = self.query_one("#wall", WallPanel)
        wall.border_title = border_text("wall [STALE]")
        wall.add_class("stale")

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

    def action_board_detail(self) -> None:
        """`b` pushes BoardDetailScreen for the next board in config.boards,
        cycling on repeat press (one board per config.boards entry, wrapping
        around) — the follower is polled only while that screen is open."""
        boards = self.config.boards
        if not boards:
            self.apply_cmd_result("no boards configured")
            return
        board = boards[self._board_cycle_index % len(boards)]
        self._board_cycle_index += 1
        self.apply_cmd_result(f"board detail: {board.name}")
        self.push_screen(BoardDetailScreen(board, self.client_factory))

    def action_log_screen(self) -> None:
        """`l` pushes the leader's flash-log screen (S3-only route)."""
        url = self.config.board_url()
        if not url:
            self.apply_cmd_result("no config — no leader url")
            return
        self.push_screen(LogScreen(url, self.client_factory))

    def action_health_screen(self) -> None:
        """`s` pushes the board-health screen (#472) — renders the last
        polled /status, so it costs no extra request."""
        self.push_screen(HealthScreen())

    def action_help(self) -> None:
        self.push_screen(HelpScreen())

    def on_input_submitted(self, event: Input.Submitted) -> None:
        if event.input.id != "command":
            return
        line = event.value
        event.input.remember(line)
        save_history(event.input.history, self.history_path)
        event.input.dismiss_bar()
        if line.strip() in ("help", "?"):
            self.push_screen(HelpScreen())
            return
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
            name = DISPLAY_NAMES.get(parsed.name, parsed.name)
            self.apply_cmd_result(f"⛔ {name}: not served on {self.plat}")
            return False
        return True

    @staticmethod
    def _typed_confirm_token(parsed: ParsedCommand) -> str:
        """#441 finding 4: reboot's token is the board name when one was
        given (`reboot row0` -> "row0"), else the command name ("reboot"),
        matching every other typed-confirm command's fallback."""
        if "board" in parsed.args:
            return parsed.args["board"] or parsed.name
        return str(parsed.args.get("unit", parsed.name))

    def dispatch_command(self, parsed: ParsedCommand) -> None:
        if not self._capability_gate(parsed):
            return
        if parsed.name == "discover":
            # #469: the only command whose result is a table rather than a
            # status line, and the only one that takes seconds to answer —
            # its screen owns the staged POST/GET scan (and the rescan key)
            # instead of execute() returning a string. Deliberately AFTER
            # the capability gate, so an esp01 still rejects it client-side.
            url = self.config.board_url()
            if not url:
                self.apply_cmd_result("no config — no leader url")
                return
            self.push_screen(DiscoverScreen(url, self.client_factory))
            return
        clock_guard = parsed.name == "text" and self.device_mode == "clock"
        if parsed.tier in (TIER_KILL, TIER_ROUTINE) and not clock_guard:
            self.run_command(parsed)
            return
        typed = parsed.tier == TIER_TYPED
        token = self._typed_confirm_token(parsed) if typed else ""
        summary = parsed.summary
        if clock_guard:
            summary = (f"{parsed.summary}\n"
                       "wall is in clock mode — the clock reclaims the "
                       "display at the next minute tick.\n"
                       "Send anyway? (tip: `:mode text` first)")

        def on_result(confirmed: bool) -> None:
            if confirmed:
                self.run_command(parsed)
            else:
                self.apply_cmd_result("cancelled")

        self.push_screen(ConfirmModal(summary, typed=typed, token=token),
                         on_result)

    def run_command(self, parsed: ParsedCommand) -> None:
        def work() -> None:
            t0 = time.monotonic()
            try:
                with self.client_factory(self.config.board_url()) as client:
                    result = execute(parsed, client, self.config, self.client_factory)
            except SplitflapError as exc:
                result = f"⛔ {exc}"
            self.call_from_thread(self.apply_cmd_result, result,
                                  time.monotonic() - t0)
        self.run_worker(work, thread=True)

    def apply_cmd_result(self, text: str, duration_s: float | None = None) -> None:
        self._last_cmd_result = text
        self.query_one("#cmd-status", Static).update(
            format_cmd_status(text, duration_s, datetime.now().strftime("%H:%M:%S")))

    def on_unmount(self) -> None:
        if self.poller:
            self.poller.stop()
