"""Pure command-bar parser + tier table — no I/O. See app.py's execute() for
the dispatch that actually talks to a board (it needs the client + op
polling, so it can't live here)."""
from __future__ import annotations

from dataclasses import dataclass

TIER_KILL = "kill"
TIER_ROUTINE = "routine"
TIER_CONFIRM = "confirm"
TIER_TYPED = "typed"

CANONICAL_NAMES = ("stop", "text", "mode", "notify", "set", "op", "gates",
                   "reboot", "reset-units", "addr", "promote",
                   "cluster-leave", "config", "discover", "baseline")


@dataclass(frozen=True)
class HelpEntry:
    usage: str
    tier: str
    blurb: str
    example: str      # must parse; the drift test derives coverage from it


HELP: list[HelpEntry] = [
    HelpEntry("stop", TIER_KILL, "blank and halt the wall NOW (also ctrl+s)", "stop"),
    HelpEntry("text <display text>", TIER_ROUTINE, "render text on the wall", "text HELLO"),
    HelpEntry("mode text|clock", TIER_ROUTINE, "switch device mode", "mode clock"),
    HelpEntry("notify <dwell-s> <text>", TIER_ROUTINE, "show text, then revert", "notify 10 DOOR"),
    HelpEntry("set <field> <value>", TIER_CONFIRM, "write one settings field", "set deviceMode clock"),
    HelpEntry("op home|jog|identify|self-test|reset-odometer|offset <unit> [value]",
              TIER_CONFIRM, "run a unit maintenance op", "op home 1"),
    HelpEntry("gates <unit> <mask>", TIER_CONFIRM, "set unit feature-gate byte", "gates 1 0x03"),
    HelpEntry("reboot [board]", TIER_TYPED, "reboot the default or named board", "reboot"),
    HelpEntry("reset-units", TIER_TYPED, "power-cycle every unit", "reset-units"),
    HelpEntry("addr burn <unit> <target> | addr clear <unit>", TIER_TYPED,
              "provision/clear a unit I2C address", "addr clear 1"),
    HelpEntry("promote", TIER_TYPED, "promote this board to cluster leader", "promote"),
    HelpEntry("cluster leave", TIER_TYPED, "leave the cluster", "cluster leave"),
    HelpEntry("cluster config <host|row|col|width;...>", TIER_TYPED,
              "replace the cluster member table", "cluster config |1|0|16;"),
    HelpEntry("discover", TIER_ROUTINE, "scan the LAN for boards (leader-side mDNS)",
              "discover"),
    HelpEntry("baseline [clear]", TIER_ROUTINE,
              "snapshot unit wear so servicing is measurable", "baseline"),
]


class CommandError(Exception):
    pass


@dataclass(frozen=True)
class ParsedCommand:
    name: str                 # canonical: stop|text|mode|notify|set|op|gates|
                               # reboot|promote|reset-units|addr|cluster-leave|
                               # config (== "cluster config <table>")
    args: dict
    tier: str
    route: tuple[str, str]    # (method, path) for capability gating
    summary: str              # human line echoed in the confirm prompt


OP_ROUTES = {"home": "/unit/home", "jog": "/unit/jog",
             "identify": "/unit/identify", "self-test": "/unit/self-test",
             "reset-odometer": "/unit/reset-odometer", "offset": "/unit/offset"}
OPS_NEED_VALUE = {"jog", "offset"}


def _int_arg(tokens: list[str], index: int, what: str) -> int:
    try:
        return int(tokens[index], 0)
    except (IndexError, ValueError):
        raise CommandError(f"usage: expected {what}") from None


def parse(line: str) -> ParsedCommand:
    tokens = line.strip().split()
    if not tokens:
        raise CommandError("empty command")
    head, rest = tokens[0], tokens[1:]

    if head == "stop":
        return ParsedCommand("stop", {}, TIER_KILL, ("POST", "/stop"),
                             "STOP — blank and halt the wall")
    if head == "text":
        text = line.strip()[len("text "):] if rest else ""
        if not text:
            raise CommandError("usage: text <display text>")
        return ParsedCommand("text", {"text": text}, TIER_ROUTINE,
                             ("POST", "/"), f"set text: {text!r}")
    if head == "mode":
        if rest[:1] not in (["text"], ["clock"]):
            raise CommandError("usage: mode text|clock")
        return ParsedCommand("mode", {"mode": rest[0]}, TIER_ROUTINE,
                             ("POST", "/"), f"mode {rest[0]}")
    if head == "notify":
        dwell = _int_arg(rest, 0, "dwell seconds")
        text = " ".join(rest[1:])
        if not text:
            raise CommandError("usage: notify <dwell-s> <text>")
        return ParsedCommand("notify", {"dwell": dwell, "text": text},
                             TIER_ROUTINE, ("POST", "/"),
                             f"notify {dwell}s: {text!r}")
    if head == "set":
        if len(rest) < 2:
            raise CommandError("usage: set <field> <value>")
        return ParsedCommand("set", {"field": rest[0],
                                     "value": " ".join(rest[1:])},
                             TIER_CONFIRM, ("POST", "/"),
                             f"set {rest[0]} = {' '.join(rest[1:])}")
    if head == "op":
        if not rest or rest[0] not in OP_ROUTES:
            raise CommandError(f"usage: op {'|'.join(OP_ROUTES)} <unit> [value]")
        op = rest[0]
        unit = _int_arg(rest, 1, "unit address")
        args: dict = {"unit": unit}
        if op in OPS_NEED_VALUE:
            args["value"] = _int_arg(rest, 2, f"{op} value")
        # NOTE: args deliberately omits "op" — OP_ROUTES gives each op a
        # unique path, so execute() dispatches on parsed.route[1] instead.
        # (An earlier draft merged op=op into args, but that breaks
        # test_op_jog_requires_value's exact-dict equality; dropped here.)
        return ParsedCommand("op", args, TIER_CONFIRM,
                             ("POST", OP_ROUTES[op]), f"op {op} unit {unit}")
    if head == "gates":
        unit = _int_arg(rest, 0, "unit address")
        mask = _int_arg(rest, 1, "gate mask")
        return ParsedCommand("gates", {"unit": unit, "mask": mask},
                             TIER_CONFIRM, ("POST", "/unit/gates"),
                             f"gates unit {unit} mask 0x{mask:02x}")
    if head == "reboot":
        # #441 finding 4: spec's dangerous tier is `reboot [board]` — an
        # optional board name, defaulting to the configured default board
        # when omitted (see app.py's execute(), which resolves this via
        # Config.board_url(board)).
        board = rest[0] if rest else ""
        summary = f"REBOOT {board}" if board else "REBOOT the board"
        return ParsedCommand("reboot", {"board": board}, TIER_TYPED,
                             ("POST", "/reboot"), summary)
    if head == "reset-units":
        return ParsedCommand("reset-units", {}, TIER_TYPED,
                             ("POST", "/reset-units"), "RESET every unit")
    if head == "addr":
        if rest[:1] == ["burn"]:
            unit = _int_arg(rest, 1, "unit address")
            target = _int_arg(rest, 2, "target address")
            return ParsedCommand("addr", {"mode": "burn", "unit": unit,
                                          "target": target}, TIER_TYPED,
                                 ("POST", "/unit/set-address"),
                                 f"BURN address {unit} -> {target}")
        if rest[:1] == ["clear"]:
            unit = _int_arg(rest, 1, "unit address")
            return ParsedCommand("addr", {"mode": "clear", "unit": unit},
                                 TIER_TYPED, ("POST", "/unit/clear-address"),
                                 f"CLEAR address of unit {unit}")
        raise CommandError("usage: addr burn <unit> <target> | addr clear <unit>")
    if head == "promote":
        return ParsedCommand("promote", {}, TIER_TYPED,
                             ("POST", "/cluster/promote"),
                             "PROMOTE this board to leader")
    if head == "discover":
        # Read-only: it arms the LEADER's mDNS browse and reads the result
        # back, mutating nothing — routine tier, no confirm. The route is the
        # POST one so the capability gate rejects it on an esp01 (both
        # /cluster/discover routes are in ESP01_NOT_SERVED). app.py's
        # dispatch_command opens DiscoverScreen for it instead of calling
        # execute(): a scan takes seconds and returns a table, which the
        # one-line command-status bar can't hold.
        return ParsedCommand("discover", {}, TIER_ROUTINE,
                             ("POST", "/cluster/discover"),
                             "scan the LAN for boards")
    if head == "baseline":
        # Local-only (#474): writes/clears a file on THIS host and never
        # touches the wall. The route is /units/health because that is the
        # data it snapshots, and it is served on both platforms so the
        # capability gate passes; app.py's dispatch_command handles the
        # command before execute() would send anything.
        mode = rest[0] if rest else "save"
        if mode not in ("save", "clear"):
            raise CommandError("usage: baseline [clear]")
        summary = ("clear the wear baseline" if mode == "clear"
                   else "snapshot unit wear as the new baseline")
        return ParsedCommand("baseline", {"mode": mode}, TIER_ROUTINE,
                             ("GET", "/units/health"), summary)
    if head == "cluster":
        if rest[:1] == ["leave"]:
            return ParsedCommand("cluster-leave", {}, TIER_TYPED,
                                 ("POST", "/cluster/leave"), "LEAVE the cluster")
        if rest[:1] == ["config"]:
            # VERIFIED (WebCluster.cpp:483-491): POST /cluster/config takes
            # a "members" FORM param (hasParam(..., true) = POST body, not
            # query) — the `host|row|col|width;…` table string, verbatim.
            # Named "config" (not "cluster-config") so the existing
            # args.get("unit", parsed.name) typed-confirm-token fallback
            # yields the spec-mandated token "config" without a special case.
            table = line.strip()[len("cluster config "):] if len(rest) > 1 else ""
            if not table:
                raise CommandError("usage: cluster config <host|row|col|width;...>")
            return ParsedCommand("config", {"members": table}, TIER_TYPED,
                                 ("POST", "/cluster/config"),
                                 f"CLUSTER CONFIG: {table}")
        raise CommandError("usage: cluster leave | cluster config <table>")
    raise CommandError(f"unknown command: {head}")
