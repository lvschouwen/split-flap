"""Every fact the boards exchange about a unit has a place in the operator API.

UnitApiJson.h gives a unit's facts readable names. This is the table of which
name each key of the facts document (shared/UnitHealth.h's serializer) became;
it fails when the serializer gains a key that is not in it, or when a name in
it is no longer written.
"""
import re
from pathlib import Path

V2 = Path(__file__).resolve().parents[2]
SERIALIZER = (V2 / "shared/UnitHealth.h").read_text()
API = (V2 / "Master/UnitApiJson.h").read_text()

# facts key -> the name it has in GET /api/v2/unit/{row}/{address}
NAMES = {
    "a": "address", "st": "state", "fw": "status", "rev": "rev", "up": "uptimeS",
    "ut": "uptimeS", "br": "brownouts", "wd": "watchdogResets", "bc": "badCommands",
    "mc": "lastStart", "rs": "restartedWhileWatched", "hs": "homeSteps", "ae": "addressStored",
    "odo": "turns", "ofs": "offset", "de": "slips", "ds": "lastSlipSteps",
    "dp": "rehomePending", "phys": "shows", "mm": "wrongLetter", "vcc": "supplyMv",
    "vmin": "supplyMinMv", "cp": "commanded", "ram": "freeRamMin", "se": "homeExcessSteps",
    "sx": "homeExcessStepsMax", "sag": "supplyDuringLastMoveMv", "he": "hallEdgesLastTurn",
    "dw": "movesInWindow", "sb": "jammed", "rx": "received", "tx": "answered",
    "dh": "selfRepairs", "pv": "protocol", "pmm": "protocolSupported", "hf": "homeFailures",
    "gates": "gates", "sxl": "homeExcessStepsEver", "stw0": "firstHallWindow",
    "stw1": "lastHallWindow", "str0": "firstStepsPerTurn", "str1": "lastStepsPerTurn",
    "fr": "futileRehomes", "frd": "hallCheckOff", "age": "heardMsAgo", "hs2": "home",
    "misses": "missed", "stale": "lost", "err": "failed", "errAge": "failedMsAgo",
    "bv": "verdict", "bcrc": "crc32", "blv": "generation",
    "rsx": "rescuedFromBootloader", "blc": "capabilities", "blk": "lock", "blf": "fuses", "blx": "crashes",
}
# "fl" is given bit by bit; the rest is the document's frame, not a unit's fact.
SPLIT = {"fl": ["homeFailed", "hallNeverSeen", "moving"]}
FRAME = {"width", "faulty", "vccMin", "units", "i", "v"}


def serializer_keys() -> set[str]:
    start = SERIALIZER.index("inline size_t buildUnitHealthJson(")
    body = SERIALIZER[start:SERIALIZER.index("\n}\n", start)]
    return set(re.findall(r'\\"(\w+)\\":', body))


def test_every_fact_has_a_name_in_the_api():
    missing = serializer_keys() - FRAME - set(NAMES) - set(SPLIT)
    assert not missing, f"no place in UnitApiJson.h for: {sorted(missing)}"


def test_the_table_names_only_keys_the_serializer_writes():
    assert not (set(NAMES) | set(SPLIT) | FRAME) - serializer_keys()


def test_every_name_is_written_by_the_api():
    written = set(re.findall(r'\["(\w+)"\]', API))
    wanted = set(NAMES.values()) | {n for names in SPLIT.values() for n in names}
    assert not wanted - written, sorted(wanted - written)
