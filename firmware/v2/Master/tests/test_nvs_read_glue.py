"""Source gate: every NVS string read of the master asks for the key first.

Preferences logs a missing key as an error. A new board has no key at all, so
a read that does not go through nvsReadString() puts an error line on the
serial port at every first start — target glue no native test reaches.
"""
import re
from pathlib import Path

TREE = Path(__file__).resolve().parent.parent


def test_no_string_is_read_from_nvs_without_asking_for_its_key():
    direct = []
    for path in sorted(TREE.glob("*.cpp")) + sorted(TREE.glob("*.h")):
        if path.name == "NvsRead.h":
            continue
        code = re.sub(r"//[^\n]*", "", path.read_text())
        if re.search(r"\bprefs_?\s*\.\s*getString\(", code):
            direct.append(path.name)
    assert direct == []


def test_the_read_asks_before_it_reads():
    code = (TREE / "NvsRead.h").read_text()
    assert "prefs.isKey(key) ? prefs.getString(key, fallback) : fallback" in code
