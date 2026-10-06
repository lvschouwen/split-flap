"""Every key the unit facts serializer writes is one the master's reader names.

The row board sends buildUnitHealthJson's document over the wall link and the
master reads it back with UnitFactsJson.h. The native round-trip test proves
the keys it exercises; this gate catches a key added to the serializer that
neither the reader nor that test has heard of.
"""
import re
from pathlib import Path

V2 = Path(__file__).resolve().parents[2]
SERIALIZER = (V2 / "shared/UnitHealth.h").read_text()
READER = (V2 / "Master/UnitFactsJson.h").read_text()

# Restated from another key; the reader says so next to that key.
DERIVED = {"a", "ae", "pmm", "vccMin"}


def serializer_keys() -> set[str]:
    start = SERIALIZER.index("inline size_t buildUnitHealthJson(")
    body = SERIALIZER[start:SERIALIZER.index("\n}\n", start)]
    return set(re.findall(r'\\"(\w+)\\":', body))


def test_the_serializer_is_found_and_has_its_known_keys():
    keys = serializer_keys()
    assert {"width", "units", "st", "ofs", "bv", "blf", "age"} <= keys and len(keys) > 50


def test_the_reader_names_every_key_the_serializer_writes():
    named = set(re.findall(r'"(\w+)"', READER))
    missing = serializer_keys() - DERIVED - named
    assert not missing, f"UnitFactsJson.h does not read: {sorted(missing)}"


def test_the_derived_keys_are_still_written():
    assert DERIVED <= serializer_keys()
