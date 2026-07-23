#!/usr/bin/env python3
"""ext-diag-benchcheck — guided validation of the #365 unit diagnostics on hardware.

Read-only over HTTP. Auto-verifies the signals that can be checked from data alone
(baseline: no false jam/hall/drag, sane reset-cause, ext-diag live), then walks the
operator through the physical-action checks the data can't induce (power-cycle a
unit → confirm the reboot line + decoded reset-cause; stall a flap → confirm the JAM
bit + jam log line). Emits a pass/observe checklist.

The tool confirms the *resulting signal*; the physical action (pulling power, jamming
a flap) is yours.

Usage:
  ext-diag-benchcheck.py --board 192.168.15.91            # baseline + guided menu
  ext-diag-benchcheck.py --board 192.168.15.91 --baseline # auto checks only, exit
  ext-diag-benchcheck.py --board 192.168.15.91 --reboot 3 # guided reboot check, unit 3
  ext-diag-benchcheck.py --board 192.168.15.91 --jam 7    # guided jam check, unit 7

/units/health and /log/flash are browser-facing (no auth).
"""
import argparse
import json
import sys
import time
import urllib.request
import urllib.error

DRAG_EXCESS_STEPS = 200
VCC_FLOOR_MV = 4000
HTTP_TIMEOUT = 5.0

RESET = "\033[0m"
BOLD = "\033[1m"
RED = "\033[31m"
YEL = "\033[33m"
GRN = "\033[32m"
DIM = "\033[2m"

PASS = f"{GRN}PASS{RESET}"
FAIL = f"{RED}FAIL{RESET}"
OBS = f"{YEL}OBSERVE{RESET}"


def http_get(ip, path):
    req = urllib.request.Request(f"http://{ip}{path}", headers={"Accept": "*/*"})
    with urllib.request.urlopen(req, timeout=HTTP_TIMEOUT) as r:
        return r.read().decode("utf-8", "replace")


def get_health(ip):
    return json.loads(http_get(ip, "/units/health"))


def get_log(ip):
    try:
        return http_get(ip, "/log/flash")
    except (urllib.error.URLError, OSError, urllib.error.HTTPError):
        return ""


def decode_reset(mc):
    if mc is None:
        return "-"
    if mc & (1 << 2):
        return "brownout"
    if mc & (1 << 3):
        return "watchdog"
    if mc & (1 << 1):
        return "external"
    if mc & (1 << 0):
        return "power-on"
    return "unknown"


def unit_by_index(data, idx):
    for u in data.get("units", []):
        if u.get("i") == idx:
            return u
    return None


def addr_hex(u):
    a = u.get("a")
    return f"0x{a:02x}" if isinstance(a, int) else "?"


# --------------------------------------------------------------------------- #
# Baseline (auto)                                                             #
# --------------------------------------------------------------------------- #
def run_baseline(ip):
    print(f"{BOLD}Baseline check — {ip}{RESET}")
    try:
        data = get_health(ip)
    except Exception as e:  # noqa: BLE001 - operator-facing tool
        print(f"  {FAIL} could not fetch /units/health: {e}")
        return False

    units = data.get("units", [])
    if not units:
        print(f"  {OBS} width 0 / no units (headless row?) — nothing to baseline")
        return True

    any_ext = any("sb" in u for u in units)
    if not any_ext:
        print(f"  {OBS} no unit reports ext-diag yet (pre-#365 firmware) — "
              f"baseline of se/sx/sag/he/dw/sb deferred until reflash")

    ok = True
    print(f"  {DIM}unit addr   reset      ext  stall he  sx   sag  flags{RESET}")
    for u in units:
        idx = u.get("i")
        if u.get("st") != 1:
            print(f"   {idx:>2}  {addr_hex(u):<5} {DIM}state={u.get('st')} (not running) — skipped{RESET}")
            continue
        reset = decode_reset(u.get("mc"))
        has_ext = "sb" in u
        sb = u.get("sb", 0)
        he = u.get("he")
        sx = u.get("sx")
        sag = u.get("sag")
        fl = u.get("fl", 0)

        problems = []
        if has_ext and (sb & 0x01):
            problems.append("JAM bit set")
        if has_ext and isinstance(he, int) and he != 1:
            problems.append(f"hall={he}")
        if isinstance(sx, int) and sx > DRAG_EXCESS_STEPS:
            problems.append(f"drag sx={sx}")
        if isinstance(sag, int) and 0 < sag < VCC_FLOOR_MV:
            problems.append(f"low sag={sag}mV")
        if isinstance(fl, int) and fl & (1 << 1):
            problems.append("home-failed")
        if isinstance(fl, int) and fl & (1 << 2):
            problems.append("hall-never")
        if reset in ("brownout", "watchdog"):
            problems.append(f"reset={reset}")

        verdict = PASS if not problems else (FAIL if any(
            p.startswith(("JAM", "home", "hall-never")) for p in problems) else OBS)
        if verdict != PASS:
            ok = ok and (verdict != FAIL)
        ext_s = "yes" if has_ext else "no"
        line = (f"   {idx:>2}  {addr_hex(u):<5} {reset:<9}  {ext_s:<3}  "
                f"{('JAM' if sb & 1 else 'ok'):<5} "
                f"{str(he) if he is not None else '-':<3} "
                f"{str(sx) if sx is not None else '-':<4} "
                f"{str(sag) if sag is not None else '-':<4} 0x{fl:02x}"
                if isinstance(fl, int) else "")
        print(f"  {verdict} {line}")
        if problems:
            print(f"          {YEL}→ {', '.join(problems)}{RESET}")
    print(f"  {BOLD}baseline: {'clean' if ok else 'issues above'}{RESET}\n")
    return ok


# --------------------------------------------------------------------------- #
# Guided physical checks                                                      #
# --------------------------------------------------------------------------- #
def _log_has(ip, needles, since_text):
    """True if any new /log/flash line since since_text contains all-of-any needle set."""
    text = get_log(ip)
    old = set(since_text.splitlines())
    new = [ln for ln in text.splitlines() if ln not in old]
    for ln in new:
        low = ln.lower()
        if any(all(n in low for n in group) if isinstance(group, tuple) else group in low
               for group in needles):
            return ln
    return None


def guided_reboot(ip, idx):
    print(f"{BOLD}Guided reboot check — unit {idx} @ {ip}{RESET}")
    data = get_health(ip)
    u0 = unit_by_index(data, idx)
    if not u0 or u0.get("st") != 1:
        print(f"  {FAIL} unit {idx} not present/running; aborting")
        return False
    up0, br0, wd0 = u0.get("up"), u0.get("br"), u0.get("wd")
    log0 = get_log(ip)
    print(f"  before: up={up0}s brownouts={br0} watchdogs={wd0} "
          f"reset={decode_reset(u0.get('mc'))}")
    input(f"  {YEL}ACTION:{RESET} power-cycle unit {idx} ({addr_hex(u0)}), "
          f"wait for it to re-home, then press Enter…")
    time.sleep(2.0)
    # give the master a couple of heartbeat rounds to re-poll this unit
    for _ in range(20):
        data = get_health(ip)
        u1 = unit_by_index(data, idx)
        if u1 and u1.get("st") == 1 and u1.get("up") is not None and (
                up0 is None or u1.get("up") < up0):
            break
        time.sleep(2.0)
    u1 = unit_by_index(data, idx) or {}
    up1, br1, wd1 = u1.get("up"), u1.get("br"), u1.get("wd")
    reset1 = decode_reset(u1.get("mc"))
    print(f"  after:  up={up1}s brownouts={br1} watchdogs={wd1} reset={reset1}")

    dropped = isinstance(up1, int) and isinstance(up0, int) and up1 < up0
    counted = (isinstance(br1, int) and br1 != br0) or (isinstance(wd1, int) and wd1 != wd0)
    logline = _log_has(ip, [("unit", "reboot"), ("rebooted",)], log0)

    ok = dropped or counted
    print(f"  {PASS if dropped else OBS} uptime reset detected: {dropped}")
    print(f"  {PASS if counted else OBS} brownout/watchdog counter changed: {counted} "
          f"(power-cycle counts as brownout on the AVR)")
    print(f"  {PASS if logline else OBS} reboot log line: "
          f"{logline if logline else 'not seen in /log/flash (check window/rate)'}")
    print(f"  {BOLD}reboot check: {'PASS' if ok else 'inconclusive — re-check'}{RESET}\n")
    return ok


def guided_jam(ip, idx):
    print(f"{BOLD}Guided jam check — unit {idx} @ {ip}{RESET}")
    data = get_health(ip)
    u0 = unit_by_index(data, idx)
    if not u0 or u0.get("st") != 1:
        print(f"  {FAIL} unit {idx} not present/running; aborting")
        return False
    if "sb" not in u0:
        print(f"  {FAIL} unit {idx} reports no ext-diag (pre-#365 fw) — reflash first")
        return False
    log0 = get_log(ip)
    print(f"  before: stall bit = {'set' if u0.get('sb', 0) & 1 else 'clear'}")
    input(f"  {YEL}ACTION:{RESET} gently hold/jam a flap on unit {idx} "
          f"({addr_hex(u0)}) and command it a move (change the displayed text), "
          f"then press Enter…")
    time.sleep(1.5)
    hit = False
    for _ in range(15):
        data = get_health(ip)
        u1 = unit_by_index(data, idx) or {}
        if u1.get("sb", 0) & 0x01:
            hit = True
            break
        time.sleep(2.0)
    logline = _log_has(ip, [("jam",), ("stalled",)], log0)
    print(f"  {PASS if hit else OBS} stall/JAM bit (sb bit0) set: {hit}")
    print(f"  {PASS if logline else OBS} jam log line: "
          f"{logline if logline else 'not seen (a single stalled move may clear before poll)'}")
    print(f"  {BOLD}jam check: {'PASS' if hit else 'inconclusive — retry with a firmer, sustained hold'}{RESET}\n")
    return hit


def menu(ip):
    while True:
        print(f"{BOLD}Guided checks — {ip}{RESET}")
        print("  [b] baseline   [r N] reboot unit N   [j N] jam unit N   [q] quit")
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
            run_baseline(ip)
        elif c == "r" and len(choice) > 1 and choice[1].isdigit():
            guided_reboot(ip, int(choice[1]))
        elif c == "j" and len(choice) > 1 and choice[1].isdigit():
            guided_jam(ip, int(choice[1]))
        else:
            print(f"  {YEL}usage: b | r <unit> | j <unit> | q{RESET}")


def main():
    ap = argparse.ArgumentParser(description="Guided bench validation for #365 ext-diag")
    ap.add_argument("--board", default="192.168.15.91", help="board IP (default leader)")
    ap.add_argument("--baseline", action="store_true", help="auto baseline only, then exit")
    ap.add_argument("--reboot", type=int, metavar="UNIT", help="guided reboot check for UNIT")
    ap.add_argument("--jam", type=int, metavar="UNIT", help="guided jam check for UNIT")
    args = ap.parse_args()

    if args.reboot is not None:
        return 0 if guided_reboot(args.board, args.reboot) else 1
    if args.jam is not None:
        return 0 if guided_jam(args.board, args.jam) else 1

    ok = run_baseline(args.board)
    if args.baseline:
        return 0 if ok else 1
    menu(args.board)
    return 0


if __name__ == "__main__":
    sys.exit(main())
