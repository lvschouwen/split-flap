"""The drum order: alphabet.json is the single list (project 4's firmware alphabet reads it too)."""
import json
from dataclasses import dataclass
from pathlib import Path
from typing import Literal

DEFAULT = Path(__file__).with_name("alphabet.json")
FLAP_COUNT = 64
KINDS = ("blank", "char", "heart", "colour")
COLOUR_SLOTS = {"black": 1, "white": 2, "red": 3, "orange": 4, "yellow": 5,
                "green": 6, "blue": 7, "violet": 8}


@dataclass(frozen=True)
class Entry:
    kind: Literal["blank", "char", "heart", "colour"]
    value: str


def _entry(row: dict) -> Entry:
    kind, value = row.get("kind"), row.get("value", "")
    if kind not in KINDS:
        raise ValueError(f"unknown kind {kind!r}")
    if kind == "colour" and value not in COLOUR_SLOTS:
        raise ValueError(f"unknown colour {value!r}")
    if kind == "char" and len(value) != 1:
        raise ValueError(f"char entry needs one character, got {value!r}")
    return Entry(kind, value)


def load_alphabet(path: Path = DEFAULT) -> tuple[Entry, ...]:
    entries = tuple(_entry(r) for r in json.loads(Path(path).read_text()))
    if len(entries) != FLAP_COUNT:
        raise ValueError(f"need {FLAP_COUNT} entries, got {len(entries)}")
    if len(set(entries)) != FLAP_COUNT:
        raise ValueError("duplicate entries")
    return entries


def inlay_colour(entry: Entry) -> str:
    """The colour an entry's inlay prints in (blank entries have no inlay)."""
    return entry.value if entry.kind == "colour" else "white"

