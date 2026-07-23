#!/usr/bin/env python3
"""ext-diag-monitor — live /units/health dashboard for the #365 unit diagnostics.

Read-only. Polls one or more boards' GET /units/health over the VPN and renders a
per-unit table of the ext-diag telemetry (step-excess, Vcc sag, hall-edges/rev,
duty, stall) plus the reset-cause/reboot data, flagging anomalies. Optionally tails
GET /log/flash for the #322/#365 transition lines (reboot / jam / drag / hall).

Fields degrade gracefully: a unit on pre-#365 firmware omits se/sx/sag/he/dw/sb
(extDiagValid=false) and those columns show "-" until the fleet is reflashed.

Usage:
  ext-diag-monitor.py                         # poll the default leader, 3s
  ext-diag-monitor.py --boards 192.168.15.91 192.168.15.121
  ext-diag-monitor.py -i 5 --tail-log
  ext-diag-monitor.py --once                  # single snapshot, no live redraw

GET /units/health and /log/flash are browser-facing (no auth), so no credentials.
"""
import argparse
import json
import sys
import time
import urllib.request
import urllib.error

DEFAULT_BOARDS = ["192.168.15.91"]        # leader; add .121 (follower row) as needed
DRAG_EXCESS_STEPS = 200                    # EXT_DIAG_DRAG_EXCESS_STEPS (master threshold)
VCC_FLOOR_MV = 4000                        # UNIT_VCC_MIN_FLOOR_MV (#366)
HTTP_TIMEOUT = 4.0

# ANSI
RESET = "\033[0m"
DIM = "\033[2m"
BOLD = "\033[1m"
RED = "\033[31m"
YEL = "\033[33m"
GRN = "\033[32m"
CYA = "\033[36m"
CLEAR = "\033[2J\033[H"


def http_get(ip, path):
    url = f"http://{ip}{path}"
    req = urllib.request.Request(url, headers={"Accept": "*/*"})
    with urllib.request.urlopen(req, timeout=HTTP_TIMEOUT) as r:
        return r.read().decode("utf-8", "replace")


def decode_reset(mc):
    """Mirror unitResetCauseDecode: brownout>watchdog>external>power-on."""
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


STATE_NAME = {0: "silent", 1: "run", 2: "boot"}


def color(s, c):
    return f"{c}{s}{RESET}"


def fmt_unit_row(u):
    """Return (row_str, anomaly_flags list) for one unit dict."""
    i = u.get("i", "?")
    addr = u.get("a")
    addr_s = f"0x{addr:02x}" if isinstance(addr, int) else "?"
    st = u.get("st")
    state_s = STATE_NAME.get(st, str(st))
    valid = u.get("v", 0)

    flags = u.get("fl")
    anomalies = []

    # Status / reset
    reset_s = decode_reset(u.get("mc"))
    br = u.get("br")
    wd = u.get("wd")
    reboots = "-" if br is None else f"{br}/{wd}"
    if isinstance(br, int) and (br or wd):
        anomalies.append("reboots")
    if reset_s in ("brownout", "watchdog"):
        anomalies.append(reset_s)
    up = u.get("up")
    up_s = "-" if up is None else _fmt_uptime(up)

    # Flag bits (fl): bit1 home-failed, bit2 hall-never, bit5 homed, bit0 moving
    if isinstance(flags, int):
        if flags & (1 << 1):
            anomalies.append("home-failed")
        if flags & (1 << 2):
            anomalies.append("hall-never")

    # Heartbeat staleness
    if u.get("stale"):
        anomalies.append("stale")

    # ext-diag block (may be absent on pre-#365 fw)
    has_ext = "sb" in u
    se = u.get("se")
    sx = u.get("sx")
    sag = u.get("sag")
    he = u.get("he")
    dw = u.get("dw")
    sb = u.get("sb")

    se_s = "-" if se is None else str(se)
    sx_s = "-" if sx is None else str(sx)
    sag_s = "-" if sag is None else str(sag)
    he_s = "-" if he is None else str(he)
    dw_s = "-" if dw is None else str(dw)

    stall = bool(sb & 0x01) if isinstance(sb, int) else False
    stall_s = color("JAM", RED) if stall else ("-" if sb is None else "ok")
    if stall:
        anomalies.append("jam")
    if isinstance(sx, int) and sx > DRAG_EXCESS_STEPS:
        anomalies.append("drag")
        sx_s = color(sx_s, YEL)
    if has_ext and isinstance(he, int) and he != 1:
        anomalies.append("hall!=1")
        he_s = color(he_s, YEL)
    if isinstance(sag, int) and 0 < sag < VCC_FLOOR_MV:
        anomalies.append("low-sag")
        sag_s = color(sag_s, YEL)

    ext_s = "" if valid else color("(no status)", DIM)
    row = (
        f" {str(i):>2}  {addr_s:<5} {state_s:<6} {reset_s:<9} {reboots:>5} "
        f"{up_s:>7}  {se_s:>4} {sx_s:>6} {sag_s:>5} {he_s:>3} {dw_s:>4}  {stall_s:<3}"
    )
    if not has_ext and valid:
        row += color("  ext:pre-#365", DIM)
    return row, anomalies


def _fmt_uptime(sec):
    if sec >= 3600:
        return f"{sec // 3600}h{(sec % 3600) // 60:02d}m"
    if sec >= 60:
        return f"{sec // 60}m{sec % 60:02d}s"
    return f"{sec}s"


HEADER = (
    f"{BOLD} un  addr  state  reset      rb/wd      up   se     sx   sag  he   dw  stall{RESET}\n"
    f"{DIM} step-excess last/max · sag=last-move Vcc(mV) · he=hall/rev · dw=duty · JAM=stall bit{RESET}"
)


def render_board(ip):
    lines = []
    try:
        raw = http_get(ip, "/units/health")
        data = json.loads(raw)
    except (urllib.error.URLError, OSError) as e:
        return [color(f"[{ip}] unreachable: {e}", RED)], 0
    except json.JSONDecodeError as e:
        return [color(f"[{ip}] bad JSON: {e}", RED)], 0

    width = data.get("width", 0)
    faulty = data.get("faulty", 0)
    vmin = data.get("vccMin")
    reflash = data.get("reflash")
    hdr = f"{BOLD}{CYA}[{ip}]{RESET} width={width} faulty="
    hdr += color(str(faulty), RED if faulty else GRN)
    if vmin is not None:
        hdr += f" fleetVccMin={color(str(vmin)+'mV', YEL if vmin < VCC_FLOOR_MV else GRN)}"
    if reflash:
        hdr += color(f" reflash={reflash}", YEL)
    lines.append(hdr)
    lines.append(HEADER)

    all_anoms = 0
    for u in data.get("units", []):
        row, anoms = fmt_unit_row(u)
        if anoms:
            all_anoms += 1
            row += "   " + color("! " + ",".join(sorted(set(anoms))), RED)
        lines.append(row)
    if not data.get("units"):
        lines.append(color("  (no units — width 0 / headless row)", DIM))
    return lines, all_anoms


class LogTailer:
    """Fetches /log/flash and prints only lines new since the last fetch."""

    def __init__(self, ip):
        self.ip = ip
        self.seen = None  # last full text

    def poll(self):
        try:
            text = http_get(self.ip, "/log/flash")
        except (urllib.error.URLError, OSError, urllib.error.HTTPError):
            return []
        if self.seen is None:
            self.seen = text
            return []  # prime: don't dump history
        if text == self.seen:
            return []
        # Emit the tail that's new (best-effort: lines not in prior text)
        old_lines = set(self.seen.splitlines())
        new = [ln for ln in text.splitlines() if ln and ln not in old_lines]
        self.seen = text
        # Only surface diagnostic-relevant lines
        keys = ("reboot", "jam", "drag", "hall", "brownout", "watchdog",
                "home", "stale", "low-vcc", "vcc")
        return [ln for ln in new if any(k in ln.lower() for k in keys)]


def main():
    ap = argparse.ArgumentParser(description="Live /units/health ext-diag monitor (#365)")
    ap.add_argument("--boards", nargs="+", default=DEFAULT_BOARDS,
                    help="board IPs to poll (default: leader %(default)s)")
    ap.add_argument("-i", "--interval", type=float, default=3.0, help="poll seconds")
    ap.add_argument("--once", action="store_true", help="single snapshot, then exit")
    ap.add_argument("--tail-log", action="store_true",
                    help="also follow GET /log/flash for transition lines")
    ap.add_argument("--no-color", action="store_true")
    args = ap.parse_args()

    if args.no_color:
        globals().update({k: "" for k in
                          ("RESET", "DIM", "BOLD", "RED", "YEL", "GRN", "CYA", "CLEAR")})

    tailers = [LogTailer(ip) for ip in args.boards] if args.tail_log else []

    try:
        while True:
            out = []
            total_anoms = 0
            for ip in args.boards:
                lines, anoms = render_board(ip)
                total_anoms += anoms
                out.extend(lines)
                out.append("")
            stamp = time.strftime("%H:%M:%S")
            banner = f"{BOLD}ext-diag monitor{RESET}  {stamp}  "
            banner += (color(f"{total_anoms} unit(s) flagged", RED)
                       if total_anoms else color("all nominal", GRN))
            if not args.once:
                sys.stdout.write(CLEAR)
            print(banner)
            print("\n".join(out))

            if tailers:
                for t in tailers:
                    for ln in t.poll():
                        print(color(f"  log[{t.ip}] {ln}", CYA))

            if args.once:
                return 0
            time.sleep(args.interval)
    except KeyboardInterrupt:
        return 0


if __name__ == "__main__":
    sys.exit(main())
