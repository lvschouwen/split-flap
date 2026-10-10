"""Documents the preview server answers with instead of a master's (--fixtures).

A scenario is a wall in one state, as the /api/v2 documents would say it:
`documents(name)` gives {path: document}. The "faults" wall shows every reason
a unit and a board can have (tests/test_web_page.py ties that to UnitVerdict.h
and BoardVerdict.h); the others are the states of the Firmware page.

Names, revs and addresses here are made up: no real wall is described.
"""
import copy
import time

MASTER = "split-flap-000001"
REV, OLD_REV, UNIT_REV = "aaaaaaa", "bbbbbbb", "ccccccc"

# reason -> (level, a, b, the unit's state). The numbers mean what
# UnitVerdict.h says they mean for the reason.
UNIT_REASONS = {
    "working": ("working", 19557, 0, "running"),
    "no-unit": ("fault", 0, 0, "silent"),
    "not-answering": ("fault", 340, 255, "running"),
    "held-in-bootloader": ("fault", 3, 0, "bootloader"),
    "in-bootloader": ("fault", 0, 0, "bootloader"),
    "wrong-protocol": ("fault", 2, 0, "running"),
    "bootloader-damaged": ("fault", 0x1234ABCD, 0, "running"),
    "home-failed": ("fault", 4, 0, "running"),
    "hall-never": ("fault", 0, 0, "running"),
    "being-updated": ("note", 0, 0, "bootloader"),
    "finding-home": ("note", 1, 0, "running"),
    "jammed": ("note", 0, 0, "running"),
    "wrong-letter": ("note", 7, 0, "running"),
    "low-supply": ("note", 4210, 4400, "running"),
    "restarted-by-itself": ("note", 5, 1, "running"),
    "firmware-outdated": ("note", 0, 0, "running"),
    "bootloader-outdated": ("note", 0x0BADF00D, 0, "running"),
    "dragging": ("note", 61, 40, "running"),
    "hall-anomaly": ("note", 3, 0, "running"),
    "worn": ("note", 48211, 0, "running"),
    "not-read": ("note", 0, 0, "running"),
    "home-failed-before": ("note", 2, 9, "running"),
}

# reason -> (level, a, b), as BoardVerdict.h.
BOARD_REASONS = {
    "working": ("working", 4, 3069),
    "lost": ("fault", 5400, 0),
    "never-seen": ("fault", 0, 0),
    "rescue": ("fault", 0, 0),
    "bus-dead": ("fault", 3, 0),
    "update-blocked": ("fault", 3, 0),
    "units-missing": ("fault", 2, 4),
    "units-fault": ("fault", 8, 16),
    "updating": ("note", 0, 0),
    "updating-units": ("note", 0, 0),
    "away": ("note", 45, 0),
    "units-unknown": ("note", 0, 0),
    "clock-not-set": ("note", 0, 0),
    "firmware-differs": ("note", 0, 0),
    "units-note": ("note", 13, 16),
}

UNIT_FIELDS = ["address", "level", "reason", "a", "b", "state", "rev", "firmware", "bootloader",
               "supplyMv", "supplyMinMv", "shows", "turns", "offset"]


def _unit_row(address, reason):
    level, a, b, state = UNIT_REASONS[reason]
    silent = state != "running"
    return [address, level, reason, a, b, state,
            None if silent else (OLD_REV if reason == "firmware-outdated" else UNIT_REV),
            None if silent else ("outdated" if reason == "firmware-outdated" else "current"),
            "damaged" if reason == "bootloader-damaged" else
            "outdated" if reason == "bootloader-outdated" else "ok",
            None if silent else (4260 if reason == "low-supply" else 5001),
            None if silent else (4210 if reason == "low-supply" else 4979),
            None if silent else address % 40, 48211 if reason == "worn" else 106 + address, 60 + address]


def _unit_doc(board, address, reason):
    level, a, b, state = UNIT_REASONS[reason]
    doc = {"board": board, "position": address - 1, "address": address, "state": state,
           "verdict": {"level": level, "reason": reason, "a": a, "b": b,
                       "also": ["low-supply", "worn"] if reason == "dragging" else []},
           "bootloader": {"verdict": "damaged" if reason == "bootloader-damaged" else "ok",
                          "crc32": "1234abcd" if reason == "bootloader-damaged" else None},
           "addressStored": reason == "worn"}
    if state != "running":
        doc["link"] = {"heardMsAgo": 340000, "missed": 255, "lost": True, "failed": 12,
                       "failedMsAgo": 2000}
        return doc
    low = reason == "low-supply"
    doc["firmware"] = {"rev": OLD_REV if reason == "firmware-outdated" else UNIT_REV,
                       "status": "outdated" if reason == "firmware-outdated" else "current",
                       "uptimeS": 19557, "protocol": 2 if reason == "wrong-protocol" else 1,
                       "protocolSupported": reason != "wrong-protocol", "gates": 5}
    doc["power"] = {"brownouts": 5, "watchdogResets": 1, "lastStart": "brownout",
                    "restartedWhileWatched": reason == "restarted-by-itself",
                    "supplyMv": 4260 if low else 5001, "supplyMinMv": 4210 if low else 4979,
                    "freeRamMin": 1448, "supplyDuringLastMoveMv": 4190 if low else 5001}
    doc["link"] = {"badCommands": 12, "received": 3604, "answered": 4097, "selfRepairs": 2,
                   "heardMsAgo": 14326, "missed": 0, "lost": False, "failed": 3,
                   "failedMsAgo": 86000, "rescuedFromBootloader": 1,
                   "silences": {"count": 2, "lastMinutes": 7, "longestMinutes": 31, "busRestarts": 4,
                                "heardAfterBusRestart": 1, "unitRestarts": 1, "lastSawTraffic": True,
                                "lastLineHeldLow": True, "lastEnded": False,
                                "lastRestartedUnit": True, "nowMinutes": 0}}
    if reason == "not-answering":
        doc["link"].update({"heardMsAgo": 340000, "missed": 255, "lost": True})
    failed = reason in ("home-failed", "hall-never")
    doc["drum"] = {"homeSteps": 1984, "homeFailed": failed, "hallNeverSeen": reason == "hall-never",
                   "moving": reason == "finding-home", "commanded": 3,
                   "homeExcessSteps": 61 if reason == "dragging" else 0,
                   "homeExcessStepsMax": 61 if reason == "dragging" else 0,
                   "hallEdgesLastTurn": 3 if reason == "hall-anomaly" else 1,
                   "jammed": reason == "jammed", "home": "home-failed" if failed else "homed",
                   "offset": 69, "turns": 48211 if reason == "worn" else 106, "slips": 2,
                   "lastSlipSteps": 14, "shows": 7 if reason == "wrong-letter" else 3,
                   "wrongLetter": reason == "wrong-letter", "homeFailures": 4 if failed else 0,
                   "homeExcessStepsEver": 17,
                   "selfTest": {"firstHallWindow": 60, "lastHallWindow": 58,
                                "firstStepsPerTurn": 2050, "lastStepsPerTurn": 2050}}
    return doc


def _levels(reasons):
    return "".join(UNIT_REASONS[r][0][0] for r in reasons)


def _row_status(reason):
    return {"uptimeS": 3069, "heap": 34832, "heapMin": 29392, "rssi": -64, "txPowerDbm": 2,
            "busTx": 8718, "busErrors": 41 if reason == "bus-dead" else 0,
            "busDead": reason == "bus-dead", "busEpisodes": 3 if reason == "bus-dead" else 0,
            "escalations": 1, "busy": reason in ("updating", "updating-units"),
            "imageSize": 465248, "timeSynced": reason != "clock-not-set"}


def _wall():
    """The "faults" wall: (wall document, {id: board}, {(id, address): unit})."""
    reasons = list(UNIT_REASONS)
    faulty = [r for r in reasons if UNIT_REASONS[r][0] == "fault"]
    noted = [r for r in reasons if UNIT_REASONS[r][0] == "note"]
    # The master's row carries the faults, the first row board the notes.
    own = (faulty * 2)[:16]
    rows, boards, units = [], {}, {}

    def add(board_id, own_row, row, reason, unit_reasons, extra):
        level, a, b = BOARD_REASONS[reason]
        verdict = {"level": level, "reason": reason, "a": a, "b": b, "also": []}
        base = {"id": "" if own_row else board_id, "own": own_row, "row": row, "col": 0,
                "width": len(unit_reasons), "text": "FAULTS"[:len(unit_reasons)].ljust(len(unit_reasons)),
                "verdict": verdict, "unitLevels": _levels(unit_reasons), **extra}
        rows.append(base)
        table = {"fields": UNIT_FIELDS,
                 "rows": [_unit_row(i + 1, r) for i, r in enumerate(unit_reasons)]}
        boards[board_id] = {**base, "id": board_id, "kind": "master" if own_row else "row",
                            "units": table, "jobRunning": reason in ("updating", "updating-units")}
        for i, r in enumerate(unit_reasons):
            units[(board_id, i + 1)] = _unit_doc(board_id, i + 1, r)

    add(MASTER, True, 0, "units-fault", own, {})
    boards[MASTER].update({
        "address": "192.0.2.10", "rev": REV, "mqttConnected": False,
        "stats": {"now": {"rssi": -81, "txPower": 20, "heap": 21756, "minHeap": 9672, "temp": 712,
                          "uptime": 2135, "i2cTx": 6040, "i2cErr": 412, "ntpAge": 2127},
                  "hist": {"rssi": [-70 - (i * 7) % 15 for i in range(120)]}, "interval": 5},
        "starts": [{"reset": "Software reset", "stage": "online"}, {"reset": "Power on", "stage": "online"},
                   {"reset": "Brownout", "stage": "wifi"}, {"reset": "Panic", "stage": "boot"}],
        "lastStart": {"reset": "Panic", "cause": "LoadProhibited in displayTask"},
        "network": {"gw": "fail", "self": "ok"},
        "rescue": {"rev": "", "state": "missing", "warn": True},
        "settings": {"name": "", "unitCount": 0}})
    at = 1
    for reason in BOARD_REASONS:
        if reason in ("units-fault",):
            continue
        board_id = f"split-flap-{at + 1:06d}"
        unit_reasons = (noted if reason == "units-note" else
                        ["working", "working", "no-unit", "no-unit"] if reason == "units-missing"
                        else ["working"] * 4)
        gone = reason in ("lost", "never-seen", "away")
        add(board_id, False, at, reason, unit_reasons, {
            "pairedAt": f"192.0.2.{20 + at}", "address": f"192.0.2.{20 + at}",
            "reach": "down" if gone else "up", "connects": 0 if reason == "never-seen" else 3,
            "restarts": 2, "heardMsAgo": 5400000 if reason == "lost" else 45000 if gone else 1904,
            "rev": OLD_REV if reason in ("firmware-differs", "update-blocked", "rescue") else REV,
            "rescue": reason == "rescue", "units": len(unit_reasons),
            "lastLateMs": 380 if reason == "clock-not-set" else 0,
            "worstLateMs": 2140 if reason == "clock-not-set" else 0,
            "updateAttempts": 3 if reason == "update-blocked" else 0,
            "updateBlocked": reason == "update-blocked",
            "status": None if reason == "never-seen" else _row_status(reason)})
        if reason in ("units-unknown", "never-seen", "bus-dead"):
            boards[board_id]["units"] = {"fields": UNIT_FIELDS, "rows": []}
        at += 1
    for row in rows:
        row["units"] = row["width"]
    wall = {"master": {"id": MASTER, "rev": REV, "units": 16, "verdict": rows[0]["verdict"],
                       "unitLevels": rows[0]["unitLevels"]},
            "verdict": "fault", "release": "v2099.01.02", "rows": rows,
            "rowImage": {"rev": OLD_REV, "size": 328491, "packed": True}}
    return wall, boards, units


def _firmware(scenario, boards, now):
    listed = []
    for board_id, board in boards.items():
        rows = board["units"]["rows"]
        outdated = sum(1 for r in rows if r[7] == "outdated")
        entry = {"id": board_id, "kind": board["kind"], "rev": board.get("rev"),
                 "current": board.get("rev") == REV,
                 "units": {"total": len(rows), "current": len(rows) - outdated, "outdated": outdated,
                           "unknown": 0},
                 "bootloaders": {"total": len(rows), "ok": len(rows), "outdated": 0, "damaged": 0,
                                 "unread": 0}}
        if board["kind"] == "row":
            entry.update({"rescue": board["rescue"], "updateAttempts": board["updateAttempts"],
                          "updateBlocked": board["updateBlocked"]})
        listed.append(entry)
    # The boards that carry what the wall's bootloader tally counts.
    listed[1]["bootloaders"].update({"ok": listed[1]["bootloaders"]["ok"] - 1, "damaged": 1})
    listed[2]["bootloaders"].update({"outdated": 2})
    release = {"state": "newer", "check": True, "lookedAt": now - 7200, "channel": "stable",
               "tag": "v2099.01.02", "notes": "https://example.org/notes", "commitTime": now,
               "master": "ddddddd", "rowImage": "ddddddd", "rescue": "ddddddd", "unitRevs": "eeeeeee"}
    if scenario == "release-failed":
        release = {"state": "failed", "check": True, "lookedAt": now - 90000, "channel": "stable",
                   "why": "the release site did not answer"}
    elif scenario == "not-looked":
        release = {"state": "not-looked", "check": False, "channel": "test"}
    elif scenario == "updating":
        release["update"] = {"step": "row-image", "done": 120000, "size": 328491}
    return {"master": {"id": MASTER, "rev": REV},
            "rescue": {"rev": "", "state": "missing", "warn": True},
            "rowImage": {"rev": OLD_REV, "size": 328491, "packed": True},
            "release": release, "update": {"phase": "idle"}, "boards": listed,
            "units": {"shouldBe": UNIT_REV, "total": 60, "current": 55, "outdated": 3, "unknown": 2},
            "bootloaders": {"shouldBe": "506b3970", "total": 60, "ok": 55, "outdated": 2, "damaged": 1,
                            "unread": 2}}


def _history(boards, now):
    row = [b for b in boards if b != MASTER][0]
    kinds = [
        {"kind": "events-dropped", "a": 14},
        {"kind": "job-failed", "board": "", "unit": 0, "job": "update-from-release"},
        {"kind": "job-failed", "board": row, "unit": 3, "job": "self-test"},
        {"kind": "job-done", "board": row, "unit": 0, "job": "home-all"},
        {"kind": "update-started", "board": "", "a": 0xDDDDDDD},
        {"kind": "release-found", "board": "", "a": 0xDDDDDDD},
        {"kind": "row-event", "board": row, "event": "bus-dead", "a": (5 << 19) | (2 << 9) | 3, "b": 40},
        {"kind": "row-event", "board": row, "event": "bus-lines", "a": (31 << 16) | 0xFFFF, "b": 0},
        {"kind": "row-event", "board": row, "event": "self-restart", "a": 1, "b": 3},
        {"kind": "row-event", "board": row, "event": "low-memory", "a": 2104},
        # A row board's start is two entries, seconds apart.
        {"kind": "row-started", "board": row, "detail": 1, "a": 0xBBBBBBB, "after": 5},
        {"kind": "row-event", "board": row, "event": "started", "a": 4, "b": 4},
        # What follows a start of the master within a minute and a half.
        {"kind": "unit-reason-on", "board": "", "unit": 15, "reason": "home-failed", "a": 63, "after": 75},
        {"kind": "unit-restarted", "board": "", "unit": 4, "cause": "brownout", "a": 5, "b": 1, "after": 20},
        {"kind": "unit-reason-on", "board": "", "unit": 15, "reason": "hall-never", "after": 3},
        {"kind": "master-started", "board": "", "detail": 9, "a": 0xAAAAAAA},
        {"kind": "something-new", "board": "split-flap-999999", "unit": 2},
    ]
    for reason, (_level, a, b, _state) in UNIT_REASONS.items():
        if reason == "working":
            continue  # never a reason that starts
        kinds.append({"kind": "unit-reason-on", "board": "", "unit": 2, "reason": reason, "a": a, "b": b})
    kinds.append({"kind": "unit-reason-off", "board": "", "unit": 2, "reason": "jammed", "a": 0, "b": 0})
    for reason, (_, a, b) in BOARD_REASONS.items():
        if reason == "working":
            continue
        kinds.append({"kind": "board-reason-on", "board": row, "reason": reason, "a": a, "b": b})
    kinds.append({"kind": "board-reason-off", "board": row, "reason": "lost", "a": 0, "b": 0})
    events = []
    for i, event in enumerate(kinds):
        # The oldest entries were written before the clock was set, and days ago.
        age = 0 if i > len(kinds) - 3 else now - i * 5400
        events.append({"seq": len(kinds) - i, "time": age, "detail": 0, "unit": 0, "a": 0, "b": 0, **event})
    # "after": so many seconds after the next entry that says no such thing.
    for i, event in enumerate(events):
        if "after" in event:
            anchor = next(e for e in events[i + 1:] if "after" not in e)
            event["time"] = anchor["time"] + event["after"]
    for event in events:
        event.pop("after", None)
    return {"events": events}


SCENARIOS = ("faults", "release-failed", "not-looked", "updating")


def documents(scenario):
    """{path: document} for a scenario; a str document is served as text."""
    if scenario not in SCENARIOS:
        raise ValueError(f"no such scenario: {scenario} (there are: {', '.join(SCENARIOS)})")
    wall, boards, units = _wall()
    now = int(time.time())
    docs = {"/api/v2/wall": wall, "/api/v2/firmware": _firmware(scenario, boards, now),
            "/api/v2/history": _history(boards, now),
            "/api/v2/settings/wall": {
                "mode": "text", "quiet": True, "alignment": "left", "speed": 40,
                "timezone": "CET-1CEST,M3.5.0,M10.5.0/3", "updateUnitsAtStart": True,
                "releaseCheck": scenario != "not-looked",
                "releaseChannel": "test" if scenario == "not-looked" else "stable",
                "mqtt": {"host": "broker.example", "port": 1883, "user": "wall", "passwordSet": True}},
            "/api/v2/settings/board/" + MASTER: {"name": "", "unitCount": 0},
            "/api/v2/log": "[12] a line of a board's log\n[13] another one\n",
            "/tz.json": {"Europe/Berlin": "CET-1CEST,M3.5.0,M10.5.0/3", "Etc/UTC": "UTC0"}}
    for board_id, board in boards.items():
        docs["/api/v2/board/" + board_id] = board
    for (board_id, address), unit in units.items():
        docs[f"/api/v2/unit/{board_id}/{address}"] = unit
    return copy.deepcopy(docs)


def stream_events(docs):
    """The stream's topics for a set of documents, as (topic, document)."""
    wall = docs["/api/v2/wall"]
    settings = docs["/api/v2/settings/wall"]
    return [
        ("wall", {"mode": settings["mode"], "quiet": settings["quiet"],
                  "rows": [{"id": r["id"], "text": r["text"]} for r in wall["rows"]]}),
        ("verdict", {"wall": wall["verdict"],
                     "boards": [{"id": r["id"], "level": r["verdict"]["level"],
                                 "reason": r["verdict"]["reason"], "unitLevels": r["unitLevels"]}
                                for r in wall["rows"]]}),
        ("jobs", [{"op": 1, "name": "update-units", "state": "running", "board": wall["rows"][1]["id"]},
                  {"op": 2, "name": "self-test", "state": "failed", "board": "", "unit": 3,
                   "detail": "the drum did not come round"}]),
        ("history", {"seq": len(docs["/api/v2/history"]["events"])}),
    ]
