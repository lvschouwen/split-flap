"""Wear baselines (#474) — a local snapshot of every unit's lifetime wear,
so servicing a unit becomes measurable.

`sxl` is a LIFETIME high-water mark kept in the unit's own EEPROM. That is
what makes it trustworthy across reboots, and also what makes it useless as
a before/after: after cleaning a unit its lifetime figure still reads
whatever its worst day was, forever. The delta against a snapshot is the
number that answers "did the work help" — a serviced unit's Δ goes flat, a
failing one's keeps climbing.

This is deliberately NOT a substitute for a master-side history (#465): it
only records when an operator runs the command, whereas the master runs
around the clock. It is cheaper (no flash, no firmware change) and blinder.

File handling is best-effort throughout, exactly like command history: an
unwritable or corrupt config dir must never take the app down.
"""
from __future__ import annotations

import json
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path

from splitflap_client.models import UnitEntry, UnitsHealth

DEFAULT_BASELINE_PATH = Path.home() / ".config" / "splitflap" / "wear-baseline.json"


@dataclass(frozen=True)
class Baseline:
    saved: str                          # ISO-8601, when it was captured
    rev: str                            # leader rev at capture time
    sxl: dict[int, int] = field(default_factory=dict)   # address -> lifetime sxl


def snapshot(health: UnitsHealth, rev: str, now: str | None = None) -> Baseline:
    """Capture the current lifetime wear, keyed by I2C ADDRESS — units are
    addressed, not positional, so a re-ordered row still matches itself."""
    return Baseline(
        saved=now or datetime.now().isoformat(timespec="seconds"),
        rev=rev,
        sxl={u.address: u.sxl for u in health.units if u.sxl is not None},
    )


def delta_sxl(entry: UnitEntry, baseline: Baseline | None) -> int | None:
    """Steps-to-home accumulated since the baseline, or None when the pair
    cannot be compared."""
    if baseline is None or entry.sxl is None:
        return None
    was = baseline.sxl.get(entry.address)
    if was is None:
        return None                     # unit added or re-addressed since
    if entry.sxl < was:
        # A lifetime high-water mark cannot fall. If it did, this address is
        # no longer the same hardware (or its EEPROM was erased), so the
        # baseline is meaningless for it — report nothing rather than a
        # negative that would read as an improvement.
        return None
    return entry.sxl - was


def save_baseline(baseline: Baseline, path: Path | None = None) -> bool:
    # Resolved per call, never bound as a default argument: a default is
    # captured at import, so patching DEFAULT_BASELINE_PATH (as the test
    # suite does to stay off the operator's real file) would be silently
    # ignored. Same idiom as history.py for the same reason.
    path = path or DEFAULT_BASELINE_PATH
    payload = {"saved": baseline.saved, "rev": baseline.rev,
               "sxl": {str(a): v for a, v in baseline.sxl.items()}}
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(payload))
        return True
    except OSError:
        return False


def load_baseline(path: Path | None = None) -> Baseline | None:
    path = path or DEFAULT_BASELINE_PATH
    try:
        raw = json.loads(path.read_text())
    except (OSError, ValueError):
        return None
    if not isinstance(raw, dict) or not isinstance(raw.get("sxl"), dict):
        return None
    sxl: dict[int, int] = {}
    for addr, value in raw["sxl"].items():
        try:
            sxl[int(addr)] = int(value)
        except (TypeError, ValueError):
            continue                    # tolerate one bad entry, keep the rest
    return Baseline(saved=str(raw.get("saved", "")),
                    rev=str(raw.get("rev", "")), sxl=sxl)


def clear_baseline(path: Path | None = None) -> bool:
    path = path or DEFAULT_BASELINE_PATH
    try:
        path.unlink()
        return True
    except OSError:
        return False
