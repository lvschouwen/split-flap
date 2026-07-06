"""Resumable provisioning state — unit 9/12 on Tuesday resumes Wednesday."""
import json
import sys
from dataclasses import asdict, dataclass, field
from pathlib import Path


@dataclass
class Session:
    unit_count: int = 0
    done: list = field(default_factory=list)
    skipped: list = field(default_factory=list)
    programmer_port: str | None = None


def next_unit(s: Session) -> int | None:
    untouched = [n for n in range(1, s.unit_count + 1)
                 if n not in s.done and n not in s.skipped]
    if untouched:
        return untouched[0]
    revisit = [n for n in s.skipped if n not in s.done]
    return revisit[0] if revisit else None


def default_session_path() -> Path:
    base = Path(sys.executable).parent if getattr(sys, "frozen", False) else Path.cwd()
    return base / "split-flap-flasher.session.json"


def load_session(path: Path) -> Session | None:
    try:
        d = json.loads(path.read_text())
        return Session(**d)
    except (OSError, ValueError, TypeError):
        return None


def save_session(s: Session, path: Path) -> None:
    path.write_text(json.dumps(asdict(s), indent=2))


def clear_session(path: Path) -> None:
    """Remove a session file, e.g. after a run completes — so the next
    'Provision a new display' run doesn't silently reuse a finished session.
    """
    path.unlink(missing_ok=True)
