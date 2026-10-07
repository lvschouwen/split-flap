#!/usr/bin/env python3
"""ext-diag-benchcheck — guided check that the wall notices what happens to a unit.

Read-only, through the master's /api/v2 (a row board's units too). The
baseline lists every unit of a board with the master's verdict on it; the
guided checks walk through the two things only a hand can do — cutting a
unit's power, holding a flap — and then look for what the master should show:
the unit's own counters, its verdict, and the entry in the wall's history.

The tool confirms the resulting signal; the physical action is yours.

Usage:
  ext-diag-benchcheck.py --board <id>              # baseline + menu
  ext-diag-benchcheck.py --board <id> --baseline   # baseline only, exit code says clean
  ext-diag-benchcheck.py --board <id> --reboot 3   # guided power-cycle check, unit 3
  ext-diag-benchcheck.py --board <id> --jam 7      # guided jam check, unit 7
  ext-diag-benchcheck.py --master <addr> ...       # not the master in unit-offsets.json

A board's id is the master's own name or the id a row board paired under
(GET /api/v2/wall); without --board the master's own row is checked.
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
BOLD = "\033[1m"
RED = "\033[31m"
YEL = "\033[33m"
GRN = "\033[32m"
DIM = "\033[2m"

PASS = f"{GRN}PASS{RESET}"
FAIL = f"{RED}FAIL{RESET}"
OBS = f"{YEL}OBSERVE{RESET}"


class Wall:
    def __init__(self, master, board):
        self.master = master
        self.master_id = self.get("/api/v2/wall")["master"]["id"]
        self.board = board or self.master_id

    def get(self, path):
        with urllib.request.urlopen(f"http://{self.master}{path}", timeout=HTTP_TIMEOUT) as reply:
            return json.loads(reply.read().decode("utf-8", "replace"))

    def units(self):
        table = self.get("/api/v2/board/" + urllib.parse.quote(self.board)).get("units") or {}
        return [dict(zip(table["fields"], row)) for row in table.get("rows", [])]

    def unit(self, address):
        """The unit's document, or None when the master has no such unit."""
        try:
            return self.get(f"/api/v2/unit/{urllib.parse.quote(self.board)}/{address}")
        except urllib.error.HTTPError:
            return None

    def newest_seq(self):
        events = self.get("/api/v2/history?limit=1")["events"]
        return events[0]["seq"] if events else 0

    def events_since(self, seq, address):
        """This unit's history entries after `seq`, oldest first."""
        # The history names the master's own row "".
        board = "" if self.board == self.master_id else self.board
        events = self.get("/api/v2/history?limit=50")["events"]
        return [e for e in reversed(events)
                if e["seq"] > seq and e.get("board") == board and e.get("unit") == address]


def default_master():
    try:
        return json.loads((Path(__file__).parent / "unit-offsets.json").read_text())["master"]
    except (OSError, ValueError, KeyError):
        return None


def event_text(event):
    return f"{event['kind']} {event.get('reason') or event.get('job') or ''}".strip()


def run_baseline(wall):
    print(f"{BOLD}Baseline — {wall.board} through {wall.master}{RESET}")
    try:
        units = wall.units()
    except (urllib.error.URLError, OSError, ValueError, KeyError) as error:
        print(f"  {FAIL} the board was not read: {error}")
        return False
    if not units:
        print(f"  {OBS} the master has no unit facts of this board")
        return True
    clean = True
    print(f"  {DIM}        addr state      firmware  verdict{RESET}")
    for u in units:
        mark = {"working": PASS, "note": OBS, "fault": FAIL}.get(u["level"], OBS)
        clean = clean and u["level"] != "fault"
        why = "" if u["level"] == "working" else f"{u['reason']} ({u['a']}, {u['b']})"
        print(f"  {mark:<16} {u['address']:>3}  {u['state'] or '-':<10} "
              f"{u['firmware'] or '-':<9} {u['level'] or '-'} {why}")
    print(f"  {BOLD}baseline: {'no fault' if clean else 'faults above'}{RESET}\n")
    return clean


def guided_reboot(wall, address):
    print(f"{BOLD}Guided power-cycle check — unit {address} of {wall.board}{RESET}")
    before = wall.unit(address)
    if not before or before.get("state") != "running":
        print(f"  {FAIL} unit {address} is not running; nothing to check")
        return False
    up0 = before["firmware"].get("uptimeS")
    br0, wd0 = before["power"].get("brownouts"), before["power"].get("watchdogResets")
    seq0 = wall.newest_seq()
    print(f"  before: up {up0} s, brownouts {br0}, watchdog resets {wd0}, "
          f"last start {before['power'].get('lastStart')}")
    input(f"  {YEL}ACTION:{RESET} cut and restore the power of unit {address}, wait until "
          f"it has found home, then press Enter…")
    after = {}
    for _ in range(40):  # the master reads each unit in turn: a round takes most of a minute
        after = wall.unit(address) or {}
        up1 = after.get("firmware", {}).get("uptimeS")
        if after.get("state") == "running" and up1 is not None and (up0 is None or up1 < up0):
            break
        time.sleep(3.0)
    up1 = after.get("firmware", {}).get("uptimeS")
    power = after.get("power", {})
    br1, wd1 = power.get("brownouts"), power.get("watchdogResets")
    print(f"  after:  up {up1} s, brownouts {br1}, watchdog resets {wd1}, "
          f"last start {power.get('lastStart')}")
    restarted = isinstance(up1, int) and isinstance(up0, int) and up1 < up0
    counted = br1 != br0 or wd1 != wd0
    noted = [e for e in wall.events_since(seq0, address)
             if e["kind"] == "unit-restarted" or e.get("reason") == "restarted-by-itself"]
    print(f"  {PASS if restarted else OBS} uptime started again: {restarted}")
    print(f"  {PASS if counted else OBS} brownout or watchdog counter moved: {counted} "
          f"(a power cut counts as a brownout on the unit)")
    print(f"  {PASS if noted else OBS} entry in the wall's history: "
          f"{event_text(noted[-1]) if noted else 'none yet'}")
    ok = restarted or counted
    print(f"  {BOLD}power-cycle check: {'PASS' if ok else 'inconclusive — check again'}{RESET}\n")
    return ok


def guided_jam(wall, address):
    print(f"{BOLD}Guided jam check — unit {address} of {wall.board}{RESET}")
    before = wall.unit(address)
    if not before or before.get("state") != "running":
        print(f"  {FAIL} unit {address} is not running; nothing to check")
        return False
    if "jammed" not in before["drum"]:
        print(f"  {FAIL} unit {address} does not report its drum's diagnostics — update it first")
        return False
    seq0 = wall.newest_seq()
    print(f"  before: jammed = {before['drum']['jammed']}")
    input(f"  {YEL}ACTION:{RESET} gently hold a flap of unit {address} and have it move "
          f"(change the text shown), then press Enter…")
    hit = False
    for _ in range(30):
        now = wall.unit(address) or {}
        if now.get("drum", {}).get("jammed"):
            hit = True
            break
        time.sleep(3.0)
    noted = [e for e in wall.events_since(seq0, address) if e.get("reason") == "jammed"]
    print(f"  {PASS if hit else OBS} the unit reports a jam: {hit}")
    print(f"  {PASS if noted else OBS} entry in the wall's history: "
          f"{event_text(noted[-1]) if noted else 'none (a unit already jammed before adds none)'}")
    print(f"  {BOLD}jam check: {'PASS' if hit else 'inconclusive — hold longer and firmer'}"
          f"{RESET}\n")
    return hit


def menu(wall):
    while True:
        print(f"{BOLD}Guided checks — {wall.board}{RESET}")
        print("  [b] baseline   [r N] power-cycle unit N   [j N] jam unit N   [q] quit")
        try:
            choice = input("  > ").strip().split()
        except EOFError:
            return
        if not choice:
            continue
        c = choice[0].lower()
        if c == "q":
            return
        if c == "b":
            run_baseline(wall)
        elif c == "r" and len(choice) > 1 and choice[1].isdigit():
            guided_reboot(wall, int(choice[1]))
        elif c == "j" and len(choice) > 1 and choice[1].isdigit():
            guided_jam(wall, int(choice[1]))
        else:
            print(f"  {YEL}usage: b | r <unit> | j <unit> | q{RESET}")


def main():
    ap = argparse.ArgumentParser(description="Guided check of what the wall notices about a unit")
    ap.add_argument("--master", default=default_master(), help="the master's address")
    ap.add_argument("--board", help="board id (default: the master's own row)")
    ap.add_argument("--baseline", action="store_true", help="baseline only, then exit")
    ap.add_argument("--reboot", type=int, metavar="UNIT", help="guided power-cycle check")
    ap.add_argument("--jam", type=int, metavar="UNIT", help="guided jam check")
    args = ap.parse_args()
    if not args.master:
        ap.error("no master: give --master")
    try:
        wall = Wall(args.master, args.board)
    except (urllib.error.URLError, OSError, ValueError, KeyError) as error:
        print(f"{FAIL} no /api/v2 answer from the master at {args.master}: {error}")
        return 1

    if args.reboot is not None:
        return 0 if guided_reboot(wall, args.reboot) else 1
    if args.jam is not None:
        return 0 if guided_jam(wall, args.jam) else 1
    clean = run_baseline(wall)
    if args.baseline:
        return 0 if clean else 1
    menu(wall)
    return 0


if __name__ == "__main__":
    sys.exit(main())
