"""Best-effort command-history persistence (#451). Never raises: an
unwritable/unreadable path degrades to in-memory-only history."""
from __future__ import annotations

from pathlib import Path

DEFAULT_HISTORY_PATH = Path.home() / ".config" / "splitflap" / "history"
LIMIT = 100


def load_history(path: Path | None = None) -> list[str]:
    p = path or DEFAULT_HISTORY_PATH
    try:
        return [l for l in p.read_text().splitlines() if l.strip()][-LIMIT:]
    except (OSError, UnicodeDecodeError):
        return []


def save_history(lines: list[str], path: Path | None = None) -> None:
    p = path or DEFAULT_HISTORY_PATH
    try:
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text("\n".join(lines[-LIMIT:]) + "\n")
    except OSError:
        pass
