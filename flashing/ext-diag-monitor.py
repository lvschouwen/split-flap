#!/usr/bin/env python3
"""ext-diag-monitor — the wall's units in a terminal, as the master judges them.

Read-only. Asks the master's /api/v2 for every board of the wall (a row
board's units too) and prints one line a unit: the master's verdict with its
leading reason, state, firmware, supply, turns and offset. Nothing is judged
here: a unit is flagged when the master's verdict on it is not "working".
With --tail the new entries of the wall's history are printed as they come.

Usage:
  ext-diag-monitor.py                       # the master named in unit-offsets.json
  ext-diag-monitor.py --master 192.168.15.88 --board <id>
  ext-diag-monitor.py -i 10 --tail
  ext-diag-monitor.py --once                # one snapshot, no redraw
"""
import argparse
import json
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

HTTP_TIMEOUT = 8.0

RESET = "\033[0m"
DIM = "\033[2m"
BOLD = "\033[1m"
RED = "\033[31m"
YEL = "\033[33m"
GRN = "\033[32m"
CYA = "\033[36m"
CLEAR = "\033[2J\033[H"

LEVEL_COLOR = {"working": "GRN", "note": "YEL", "fault": "RED"}


def color(text, name):
    return f"{globals()[name]}{text}{RESET}"


def default_master():
    try:
        return json.loads((Path(__file__).parent / "unit-offsets.json").read_text())["master"]
    except (OSError, ValueError, KeyError):
        return None


def get_json(master, path):
    with urllib.request.urlopen(f"http://{master}{path}", timeout=HTTP_TIMEOUT) as reply:
        return json.loads(reply.read().decode("utf-8", "replace"))


def board_ids(wall):
    return [wall["master"]["id"]] + [r["id"] for r in wall["rows"] if not r["own"]]


def cell(value, width):
    return f"{'-' if value is None else value!s:>{width}}"


HEADER = (f"{'addr':>5} {'verdict':<8} {'reason':<32} {'state':<10} {'firmware':<9} "
          f"{'rev':<8} {'boot':<9} {'mV':>5} {'min':>5} {'turns':>6} {'offset':>6}")


def unit_line(fields, row):
    u = dict(zip(fields, row))
    level = u["level"] or "-"
    reason = u["reason"] or "-"
    if u["level"] not in (None, "working"):
        reason += f" ({u['a']}, {u['b']})"
    line = (f"{cell(u['address'], 5)} {color(f'{level:<8}', LEVEL_COLOR.get(level, 'DIM'))} "
            f"{reason:<32} {u['state'] or '-':<10} {u['firmware'] or '-':<9} "
            f"{u['rev'] or '-':<8} {u['bootloader'] or '-':<9} {cell(u['supplyMv'], 5)} "
            f"{cell(u['supplyMinMv'], 5)} {cell(u['turns'], 6)} {cell(u['offset'], 6)}")
    return line, u["level"] not in (None, "working")


def render_board(master, board_id):
    """The board's lines and how many of its units the master flags."""
    try:
        board = get_json(master, "/api/v2/board/" + urllib.parse.quote(board_id))
    except (urllib.error.URLError, OSError, ValueError) as error:
        return [color(f"[{board_id}] not read: {error}", "RED")], 0
    verdict = board.get("verdict") or {}
    level = verdict.get("level", "-")
    title = f"{BOLD}{color(f'[{board_id}]', 'CYA')}{RESET} {board.get('kind', '?')}"
    if board.get("kind") == "row":
        title += f" reach={board.get('reach', '?')}"
    title += f" rev={board.get('rev', '?')} verdict="
    title += color(f"{level} {verdict.get('reason', '')}".strip(), LEVEL_COLOR.get(level, "DIM"))
    lines = [title, color(HEADER, "DIM")]
    units = board.get("units") or {}
    flagged = 0
    for row in units.get("rows", []):
        line, flag = unit_line(units["fields"], row)
        flagged += flag
        lines.append(line)
    if not units.get("rows"):
        lines.append(color("  (the master has no unit facts of this board)", "DIM"))
    return lines, flagged


class HistoryTail:
    """Entries of GET /api/v2/history newer than the last one seen."""

    def __init__(self, master):
        self.master = master
        self.seen = None

    def poll(self):
        try:
            events = get_json(self.master, "/api/v2/history?limit=20")["events"]
        except (urllib.error.URLError, OSError, ValueError, KeyError):
            return []
        if not events:
            return []
        newest = events[0]["seq"]
        if self.seen is None:
            self.seen = newest  # what was there before the start is not news
            return []
        fresh = [e for e in events if e["seq"] > self.seen]
        self.seen = newest
        return [event_line(e) for e in reversed(fresh)]


def event_line(event):
    stamp = time.strftime("%H:%M:%S", time.localtime(event.get("time", 0)))
    where = event.get("board", "")
    if event.get("unit"):
        where += f" unit {event['unit']}"
    what = event.get("reason") or event.get("job") or event.get("detail", "")
    return f"{stamp} {event.get('kind', '?')} {where} {what} ({event.get('a')}, {event.get('b')})"


def main():
    ap = argparse.ArgumentParser(description="The wall's units as the master judges them")
    ap.add_argument("--master", default=default_master(), help="the master's address")
    ap.add_argument("--board", action="append", help="only this board id (repeatable)")
    ap.add_argument("-i", "--interval", type=float, default=5.0, help="seconds between reads")
    ap.add_argument("--once", action="store_true", help="one snapshot, then exit")
    ap.add_argument("--tail", action="store_true", help="also print new history entries")
    ap.add_argument("--no-color", action="store_true")
    args = ap.parse_args()
    if not args.master:
        ap.error("no master: give --master")
    if args.no_color:
        globals().update({k: "" for k in
                          ("RESET", "DIM", "BOLD", "RED", "YEL", "GRN", "CYA", "CLEAR")})

    tail = HistoryTail(args.master) if args.tail else None
    news = []
    try:
        while True:
            out = []
            flagged = 0
            try:
                wall = get_json(args.master, "/api/v2/wall")
                boards = args.board or board_ids(wall)
                wall_level = wall.get("verdict", "-")
            except (urllib.error.URLError, OSError, ValueError, KeyError) as error:
                boards, wall_level = [], "-"
                out.append(color(f"the master at {args.master} did not answer: {error}", "RED"))
            for board_id in boards:
                lines, count = render_board(args.master, board_id)
                flagged += count
                out.extend(lines)
                out.append("")
            banner = f"{BOLD}wall{RESET} {args.master}  {time.strftime('%H:%M:%S')}  "
            banner += color(wall_level, LEVEL_COLOR.get(wall_level, "DIM")) + "  "
            banner += (color(f"{flagged} unit(s) flagged", "RED") if flagged
                       else color("no unit flagged", "GRN"))
            if not args.once:
                sys.stdout.write(CLEAR)
            print(banner)
            print("\n".join(out))
            if tail:
                news = (news + tail.poll())[-15:]
                for line in news:
                    print(color("  " + line, "CYA"))
            if args.once:
                return 0
            time.sleep(args.interval)
    except KeyboardInterrupt:
        return 0


if __name__ == "__main__":
    sys.exit(main())
