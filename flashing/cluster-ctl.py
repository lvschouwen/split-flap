#!/usr/bin/env python3
"""cluster-ctl — a tiny stdlib-only CLI to monitor and drive a split-flap v2
cluster over its HTTP/JSON API (no Web UI required).

It talks to the S3 leader's aggregate `GET /status` and auto-discovers the
rest of the wall from `cluster.members[]`, so `status`/`watch` need only the
leader address. Mutating commands send NO `Origin` header, so the #313 CSRF
gate treats them as server-to-server and lets them through.

Examples:
  cluster-ctl status
  cluster-ctl watch --interval 2
  cluster-ctl text "HELLO WORLD"            # sets deviceMode=text on the leader
  cluster-ctl mode clock
  cluster-ctl units --target 192.168.15.121
  cluster-ctl ota --target 192.168.15.91 --bin ../firmware/v2/Master/.pio/build/master/firmware.bin
  cluster-ctl authcheck                      # scans the leader log for HMAC join markers
  cluster-ctl raw GET /cluster/status

Config: --leader (default $SF_LEADER or 192.168.15.91). Most commands act on
--target (default: the leader).
"""

import argparse
import json
import os
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

DEFAULT_LEADER = os.environ.get("SF_LEADER", "192.168.15.91")
HERE = os.path.dirname(os.path.abspath(__file__))
OTA_FLASH = os.path.join(HERE, "ota-flash.sh")

# ---- tiny ANSI helper -------------------------------------------------------

_USE_COLOR = sys.stdout.isatty() and os.environ.get("NO_COLOR") is None


def c(text, code):
    if not _USE_COLOR:
        return text
    return f"\033[{code}m{text}\033[0m"


def green(t):
    return c(t, "32")


def red(t):
    return c(t, "31")


def yellow(t):
    return c(t, "33")


def cyan(t):
    return c(t, "36")


def dim(t):
    return c(t, "2")


def bold(t):
    return c(t, "1")


# ---- HTTP -------------------------------------------------------------------


def http(host, method, path, data=None, timeout=6.0):
    """Return (status_code, body_str). data is a dict → form-urlencoded POST.
    Never sends an Origin header (server-to-server; passes the CSRF gate)."""
    url = f"http://{host}{path}"
    body = None
    headers = {}
    if data is not None:
        body = urllib.parse.urlencode(data).encode()
        headers["Content-Type"] = "application/x-www-form-urlencoded"
    req = urllib.request.Request(url, data=body, method=method, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return resp.status, resp.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode("utf-8", "replace")
    except (urllib.error.URLError, OSError, TimeoutError) as e:
        return 0, str(e)


def jget(host, path, timeout=6.0):
    st, body = http(host, "GET", path, timeout=timeout)
    if st != 200:
        return None
    try:
        return json.loads(body)
    except json.JSONDecodeError:
        return None


# ---- helpers ----------------------------------------------------------------


def fmt_uptime(seconds):
    # Both the S3 (/status stats.now `uptime`) and the ESP-01 (/settings `up`)
    # report uptime in SECONDS.
    if seconds is None:
        return "?"
    s = int(seconds)
    d, s = divmod(s, 86400)
    h, s = divmod(s, 3600)
    m, _ = divmod(s, 60)
    if d:
        return f"{d}d{h}h"
    if h:
        return f"{h}h{m}m"
    return f"{m}m"


def fmt_heap(b):
    if b is None:
        return "?"
    return f"{int(b)//1024}k"


def health_bar(detected, faulty, width):
    n = width or detected or 0
    ok = max(0, (detected or 0) - (faulty or 0))
    if n <= 0:
        return dim("(no units)")
    cells = []
    for i in range(n):
        if i < ok:
            cells.append(green("█"))
        elif i < (detected or 0):
            cells.append(red("█"))
        else:
            cells.append(dim("·"))
    tag = f"{ok}/{n} ok" if not faulty else red(f"{ok}/{n} ok ({faulty} faulty)")
    return "".join(cells) + "  " + tag


# ---- commands ---------------------------------------------------------------


def resolve_target(args):
    return args.target or args.leader


def build_member_view(leader, status):
    """Yield dicts describing each wall member from the leader's /status."""
    settings = status.get("settings", {})
    now = status.get("stats", {}).get("now", {})
    cluster = status.get("cluster", {})
    members = cluster.get("members", [])
    if not members:
        # Standalone leader — present it as a single row.
        members = [{"self": True, "row": settings.get("clusterRow", 0),
                    "width": status.get("units", {}).get("width", 0),
                    "detected": status.get("units", {}).get("width", 0),
                    "faulty": status.get("units", {}).get("faulty", 0)}]
    out = []
    for m in members:
        is_self = m.get("self")
        host = leader if is_self else m.get("host", "")
        row = {
            "host": host,
            "self": bool(is_self),
            "row": m.get("row"),
            "width": m.get("width"),
            "detected": m.get("detected"),
            "faulty": m.get("faulty"),
            "wear": m.get("wear"),
            "rev": m.get("rev"),
            "plat": m.get("plat", "esp32" if is_self else "?"),
            "degraded": m.get("degraded"),
            "joined": m.get("joined", True),
            "updating": m.get("updating"),
            "name": None,
            "heap": None,
            "rssi": None,
            "up": None,
            "phase": "leader" if is_self else None,
            "extra": None,
            "text": None,
        }
        if is_self:
            row["name"] = settings.get("effectiveDeviceName") or settings.get("deviceName")
            row["heap"] = now.get("heap")
            row["rssi"] = now.get("rssi")
            row["up"] = now.get("uptime")
            row["rev"] = row["rev"] or settings.get("version")
            row["text"] = settings.get("lastWrittenText")  # leader's own row
            # The leader is the data-rich node — surface its full vitals.
            u = status.get("units", {})
            temp = now.get("temp")
            row["extra"] = {
                "temp": f"{temp/10:.1f}C" if temp is not None else None,
                "psram": fmt_heap(now.get("psram")),
                "minHeap": fmt_heap(now.get("minHeap")),
                "cpu": (f"{now.get('cpu0')}/{now.get('cpu1')}%"
                        if now.get("cpu0") is not None else None),
                "vccMin": (f"{u.get('vccMin')}mV" if u.get("vccMin") else None),
                "i2cErr": now.get("i2cErr"),
                "ntpAge": (fmt_uptime(now.get("ntpAge"))
                           if now.get("ntpAge") is not None else None),
                "mqttDrops": now.get("mqttDrops"),
                "reset": now.get("reset"),
            }
        else:
            fs = jget(host, "/settings", timeout=3.0) if host else None
            if fs:
                row["name"] = fs.get("effectiveDeviceName") or fs.get("deviceName")
                row["heap"] = fs.get("heap")
                row["rssi"] = fs.get("rssi")
                row["up"] = fs.get("up")  # seconds (ApiIndex)
                row["phase"] = fs.get("clusterState")
                row["rev"] = row["rev"] or fs.get("version")
                row["extra"] = {"minHeap": fmt_heap(fs.get("minHeap"))
                                if fs.get("minHeap") else None}
            # The rendered segment for this row lives in /cluster/health.
            ch = jget(host, "/cluster/health", timeout=3.0) if host else None
            if ch:
                row["text"] = ch.get("segment")
        out.append(row)
    out.sort(key=lambda r: (r["row"] if r["row"] is not None else 99))
    return out, settings, now


def render_dashboard(leader):
    status = jget(leader, "/status")
    if not status:
        return f"{red('UNREACHABLE')}  leader {leader} did not answer GET /status"
    members, settings, now = build_member_view(leader, status)
    lines = []
    leading = settings.get("clusterLeading")
    hdr = f"{bold('split-flap cluster')}  ·  leader {cyan(leader)}"
    hdr += "  ·  " + ("LEADING" if leading else dim("standalone/not-leading"))
    hdr += "  ·  " + dim(time.strftime("%H:%M:%S"))
    lines.append(hdr)
    lines.append("")
    for m in members:
        dot = green("●") if not m["degraded"] and m["joined"] else red("●")
        name = m["name"] or dim("(unknown)")
        role = "S3" if m["plat"] in ("esp32", "?") and m["self"] else (m["plat"] or "?")
        phase = m["phase"] or ("joined" if m["joined"] else red("not-joined"))
        rev = m["rev"] or "?"
        upd = yellow("  UPDATING") if m["updating"] else ""
        vit = []
        if m["heap"] is not None:
            vit.append(f"heap {fmt_heap(m['heap'])}")
        if m["rssi"] is not None:
            vit.append(f"rssi {m['rssi']}")
        if m["up"] is not None:
            vit.append(f"up {fmt_uptime(m['up'])}")
        vitstr = ("  " + dim("  ".join(vit))) if vit else ""
        wear = yellow("  ⚠wear") if m["wear"] else ""
        lines.append(
            f"{dot} {bold(name):<28} {role:<6} {phase:<11} rev {cyan(rev)}{vitstr}{upd}{wear}")
        lines.append(
            f"   row{m['row']}  " + health_bar(m["detected"], m["faulty"], m["width"]))
        extra = m.get("extra") or {}
        labels = [("temp", "temp"), ("cpu", "cpu"), ("psram", "psram free"),
                  ("minHeap", "minheap"), ("vccMin", "vccMin"),
                  ("i2cErr", "i2cErr"), ("ntpAge", "ntp-age"),
                  ("mqttDrops", "mqttDrops")]
        parts = [f"{lbl} {extra[key]}" for key, lbl in labels
                 if extra.get(key) is not None]
        if parts:
            lines.append(dim("        " + "   ".join(parts)))
        if extra.get("reset") and extra["reset"] not in ("", "Unknown"):
            rst = extra["reset"]
            tag = red(rst) if "rown" in rst or "anic" in rst or "atchdog" in rst else dim(rst)
            lines.append(dim("        last reset: ") + tag)
    lines.append("")
    mode = settings.get("deviceMode", "?")
    speed = settings.get("flapSpeed", "?")
    lines.append(bold("wall content") + dim(f"   mode={mode}  speed={speed}  "
                                             f"units={settings.get('unitCount')}"
                                             f" (override {settings.get('unitCountOverride')})"))
    # Each row's actually-rendered text, boxed to its width, stacked like the
    # physical wall — so a 2-row wall reads as two rows here too.
    for m in members:
        w = m.get("width") or 0
        t = m.get("text")
        who = "S3/leader" if m["self"] else (m.get("plat") or "")
        if t is None:
            cell = "│" + " " * w + "│  " + red("(no content — unreachable?)")
        else:
            shown = t[:w] if w else t
            cell = "│" + cyan(shown.ljust(w)) + "│"
        lines.append(f"  row{m['row']} {cell}  {dim(who)}")
    ota = status.get("ota", {})
    if ota.get("otaReverted"):
        lines.append(red("  ⚠ OTA reverted — last image rolled back"))
    if settings.get("recoveryMode"):
        lines.append(red("  ⚠ recovery mode"))
    return "\n".join(lines)


def cmd_status(args):
    print(render_dashboard(args.leader))


def cmd_watch(args):
    try:
        while True:
            out = render_dashboard(args.leader)
            sys.stdout.write("\033[2J\033[H" if _USE_COLOR else "\n" * 2)
            print(out)
            print(dim(f"\n(refresh {args.interval}s — Ctrl-C to stop)"))
            time.sleep(args.interval)
    except KeyboardInterrupt:
        print()


def cmd_units(args):
    host = resolve_target(args)
    d = jget(host, "/units/health")
    if not d:
        print(red(f"no unit health from {host}"))
        return
    units = d.get("units", [])
    print(bold(f"{host}  width={d.get('width')}  faulty={d.get('faulty')}  "
               f"vccMin={d.get('vccMin')}mV"))
    print(dim("  i addr st  fw       homed  vcc(min)  odo    age"))
    for u in units:
        homed = green("yes") if u.get("hs2") == 2 else red("no")
        st = u.get("st")
        stc = green(str(st)) if st == 1 else red(str(st))
        print(f"  {u.get('i'):>2} 0x{u.get('a'):02x} {stc:<3} {str(u.get('rev','?')):<8} "
              f"{homed:<6} {u.get('vcc')}({u.get('vmin')})  {u.get('odo')}   {u.get('age')}")


def cmd_text(args):
    host = resolve_target(args)
    data = {"inputText": args.message, "deviceMode": "text", "ajax": "1"}
    st, body = http(host, "POST", "/", data)
    _report_post(host, "set text", st, body)


def cmd_mode(args):
    host = resolve_target(args)
    if args.mode not in ("text", "clock"):
        print(red("mode must be text|clock"))
        sys.exit(2)
    st, body = http(host, "POST", "/", {"deviceMode": args.mode, "ajax": "1"})
    _report_post(host, f"set mode={args.mode}", st, body)


def cmd_stop(args):
    host = resolve_target(args)
    st, body = http(host, "POST", "/stop", {})
    _report_post(host, "stop/blank", st, body)


def cmd_reboot(args):
    host = resolve_target(args)
    st, body = http(host, "POST", "/reboot", {})
    _report_post(host, "reboot", st, body)


def cmd_promote(args):
    host = resolve_target(args)
    if host == args.leader and not args.force:
        print(red("refusing to promote the current leader; use --target <follower> "
                  "(or --force)"))
        sys.exit(2)
    st, body = http(host, "POST", "/cluster/promote", {})
    _report_post(host, "promote", st, body)


def cmd_leave(args):
    host = resolve_target(args)
    st, body = http(host, "POST", "/cluster/leave", {})
    _report_post(host, "leave cluster", st, body)


def cmd_cluster(args):
    d = jget(args.leader, "/cluster/status")
    if not d:
        print(red("no /cluster/status"))
        return
    print(json.dumps(d, indent=2))


def cmd_authcheck(args):
    """Scan the leader's persistent flash log for HMAC join markers — the one
    observable signal that wire-auth negotiated (the leader logs '[authenticated]'
    per member on a keyed join; the ESP-01 has no serial to read)."""
    st, body = http(args.leader, "GET", "/log/flash", timeout=8.0)
    if st != 200:
        print(red(f"no /log/flash ({st})"))
        return
    hits = [ln for ln in body.splitlines() if "authenticated" in ln.lower()
            or "joined by" in ln.lower()]
    if not hits:
        print(yellow("no join markers in the flash log yet "
                     "(no recent (re)join, or pre-HMAC firmware)"))
        return
    print(bold("leader join markers (most recent last):"))
    for ln in hits[-12:]:
        mark = green("✓ authenticated") if "authenticated" in ln.lower() else yellow("· unsigned")
        print(f"  {mark}  {dim(ln.strip())}")


def cmd_log(args):
    host = resolve_target(args)
    path = "/log/flash" if args.flash else "/log"
    st, body = http(host, "GET", path, timeout=8.0)
    if st != 200:
        hint = ("  (the ESP-01 has no /log endpoint yet — see issue #316; "
                "the S3 serves /log and /log/flash)" if st in (404, 500) else "")
        print(yellow(f"no log from {host}{path} (HTTP {st}){hint}"))
        return
    # /log may be plain text or a JSON wrapper — handle both.
    lines = body.splitlines()
    try:
        j = json.loads(body)
        if isinstance(j, dict):
            for k in ("log", "lines", "text"):
                if k in j:
                    v = j[k]
                    lines = v if isinstance(v, list) else str(v).splitlines()
                    break
    except json.JSONDecodeError:
        pass
    tail = lines[-args.n:] if args.n and args.n > 0 else lines
    print(bold(f"{host}{path}  ({len(lines)} lines, showing {len(tail)})"))
    for ln in tail:
        print("  " + str(ln).rstrip())


def cmd_ota(args):
    if not os.path.exists(args.bin):
        print(red(f"bin not found: {args.bin}"))
        sys.exit(2)
    host = resolve_target(args)
    cmd = ["bash", OTA_FLASH, "-y", "-l", args.bin]
    if args.plat:
        cmd += ["-p", args.plat]
    cmd.append(host)
    print(cyan("$ " + " ".join(cmd)))
    rc = subprocess.call(cmd)
    sys.exit(rc)


def cmd_raw(args):
    host = resolve_target(args)
    data = None
    if args.data:
        data = dict(kv.split("=", 1) for kv in args.data)
    st, body = http(host, args.method.upper(), args.path, data)
    print(f"{st}")
    print(body)


def _report_post(host, what, st, body):
    if st == 200:
        print(green(f"✓ {what} → {host}  ({body.strip() or 'ok'})"))
    elif st == 409:
        print(yellow(f"⚠ {what} → {host}: 409 {body.strip()} "
                     "(clustered follower? set text on the leader instead)"))
    elif st == 0:
        print(red(f"✗ {what} → {host}: unreachable ({body})"))
    else:
        print(red(f"✗ {what} → {host}: {st} {body.strip()}"))


# ---- argparse ---------------------------------------------------------------


def main():
    # Shared options live on a parent applied to each SUBparser only. Defining
    # them on the top parser too would let `--target` appear before the
    # subcommand, but argparse would then clobber it back to the subparser's
    # None default — so require them after the subcommand: `units --target X`.
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--leader", default=DEFAULT_LEADER,
                        help=f"S3 leader host (default {DEFAULT_LEADER})")
    common.add_argument("--target", help="host to act on (default: the leader)")

    p = argparse.ArgumentParser(prog="cluster-ctl", description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True, parser_class=lambda **kw:
                           argparse.ArgumentParser(parents=[common], **kw))

    sub.add_parser("status", help="one-shot cluster dashboard").set_defaults(fn=cmd_status)
    w = sub.add_parser("watch", help="live-refreshing dashboard")
    w.add_argument("--interval", type=float, default=2.0)
    w.set_defaults(fn=cmd_watch)
    sub.add_parser("cluster", help="raw /cluster/status JSON").set_defaults(fn=cmd_cluster)
    sub.add_parser("units", help="per-unit health table").set_defaults(fn=cmd_units)
    sub.add_parser("stop", help="blank + halt the display").set_defaults(fn=cmd_stop)
    sub.add_parser("reboot", help="soft reboot").set_defaults(fn=cmd_reboot)
    sub.add_parser("leave", help="leave the cluster").set_defaults(fn=cmd_leave)
    sub.add_parser("authcheck",
                   help="scan leader log for HMAC join markers").set_defaults(fn=cmd_authcheck)

    lg = sub.add_parser("log", help="tail a device log (S3 only; --flash = persistent)")
    lg.add_argument("--flash", action="store_true",
                    help="persistent /log/flash instead of the RAM /log")
    lg.add_argument("-n", type=int, default=40, help="last N lines (default 40)")
    lg.set_defaults(fn=cmd_log)

    t = sub.add_parser("text", help="set display text (deviceMode=text)")
    t.add_argument("message")
    t.set_defaults(fn=cmd_text)

    m = sub.add_parser("mode", help="set deviceMode text|clock")
    m.add_argument("mode")
    m.set_defaults(fn=cmd_mode)

    pr = sub.add_parser("promote", help="promote a follower to leader")
    pr.add_argument("--force", action="store_true")
    pr.set_defaults(fn=cmd_promote)

    o = sub.add_parser("ota", help="OTA a device (wraps ota-flash.sh -l)")
    o.add_argument("--bin", required=True)
    o.add_argument("--plat", choices=["esp01", "esp32"],
                   help="assert platform (autodetected from /settings otherwise)")
    o.set_defaults(fn=cmd_ota)

    r = sub.add_parser("raw", help="raw HTTP call")
    r.add_argument("method")
    r.add_argument("path")
    r.add_argument("--data", nargs="*", help="k=v form fields for a POST")
    r.set_defaults(fn=cmd_raw)

    args = p.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()
