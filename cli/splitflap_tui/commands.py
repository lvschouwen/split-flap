"""Pure command-bar parser + tier table — no I/O. See app.py's execute() for
the dispatch that actually talks to a board (it needs the client + op
polling, so it can't live here)."""
from __future__ import annotations

from dataclasses import dataclass

TIER_KILL = "kill"
TIER_ROUTINE = "routine"
TIER_CONFIRM = "confirm"
TIER_TYPED = "typed"


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
        return ParsedCommand("reboot", {}, TIER_TYPED, ("POST", "/reboot"),
                             "REBOOT the board")
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
