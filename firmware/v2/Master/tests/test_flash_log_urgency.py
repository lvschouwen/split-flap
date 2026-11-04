"""Drift gate for the flash log's early flush (#628).

flashLogTextUrgent (FlashLogPolicy.h) decides by words whether a staged line
is a warning or an error. Every log literal of this tree that carries one of
its words is listed in tests/flash_log_urgency.txt with the verdict decided
for it, so a new line with such a word fails here until someone decides.
The native test_flash_log_policy runs the classifier itself over the same
table (test/test_flash_log_policy/urgency_samples.h, generated from it), so
the table and the C++ cannot drift apart. The classifier is not ported here:
this side only finds the literals, loosely, by substring.

Regenerate the header after editing the table:
    python3 tests/test_flash_log_urgency.py --write-header
List literals the table lacks:
    python3 tests/test_flash_log_urgency.py --missing
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TABLE = ROOT / "tests" / "flash_log_urgency.txt"
HEADER = ROOT / "test" / "test_flash_log_policy" / "urgency_samples.h"

_LITERAL = r'"(?:[^"\\\n]|\\.)*"'
_CALL = re.compile(
    r"\bSerial(?:Printf|Println|Print)\s*\(\s*(?:F\s*\(\s*)?((?:" + _LITERAL + r"\s*)+)")
_CONVERSION = re.compile(r"%[-+ #0]*\d*(?:\.\d+)?(?:hh|h|ll|l|z)?([diuxXsc%])")


def keywords():
    policy = (ROOT / "FlashLogPolicy.h").read_text()
    body = re.search(r"FLASH_LOG_URGENT_WORDS\[\]\s*=\s*\{(.*?)\};", policy, re.S)
    words = re.findall(r'"([^"]+)"', body.group(1))
    assert words
    return words


def logged_literals():
    words = keywords()
    found = set()
    for path in sorted(list(ROOT.glob("*.cpp")) + list(ROOT.glob("*.h"))):
        code = re.sub(r"//[^\n]*", "", path.read_text())
        for match in _CALL.finditer(code):
            text = "".join(re.findall(r'"((?:[^"\\\n]|\\.)*)"', match.group(1)))
            if any(w in text.lower() for w in words):
                found.add(text)
    return found


def table():
    entries = {}
    for line in TABLE.read_text().splitlines():
        if not line.strip() or line.startswith("#"):
            continue
        verdict, text = line.split("\t", 1)
        assert verdict in ("urgent", "routine"), line
        assert text not in entries, f"listed twice: {text}"
        entries[text] = verdict == "urgent"
    return entries


def _rendered(text):
    # A sample as the line is logged: numbers for numeric conversions.
    return _CONVERSION.sub(
        lambda m: {"s": "", "c": "x", "%": "%"}.get(m.group(1), "3"), text)


def header_text():
    rows = "".join(f'    {{"{_rendered(t)}", {"true" if u else "false"}}},\n'
                   for t, u in sorted(table().items()))
    return (
        "// Generated from tests/flash_log_urgency.txt by\n"
        "// tests/test_flash_log_urgency.py --write-header. Do not edit.\n"
        "#pragma once\n\n"
        "struct FlashLogUrgencySample {\n"
        "  const char* text;\n"
        "  bool urgent;\n"
        "};\n\n"
        "static const FlashLogUrgencySample FLASH_LOG_URGENCY_SAMPLES[] = {\n"
        + rows + "};\n")


def test_every_logged_literal_with_a_keyword_has_a_decided_verdict():
    missing = sorted(logged_literals() - set(table()))
    assert not missing, "decide urgent/routine in tests/flash_log_urgency.txt:\n" + \
        "\n".join(missing)


def test_the_table_lists_only_literals_still_logged():
    stale = sorted(set(table()) - logged_literals())
    assert not stale, "no longer logged:\n" + "\n".join(stale)


def test_the_native_samples_are_generated_from_the_table():
    assert HEADER.read_text() == header_text(), \
        "run: python3 tests/test_flash_log_urgency.py --write-header"


if __name__ == "__main__":
    if "--write-header" in sys.argv:
        HEADER.write_text(header_text())
    elif "--missing" in sys.argv:
        known = set(table()) if TABLE.exists() else set()
        for text in sorted(logged_literals() - known):
            print(text)
