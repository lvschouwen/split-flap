"""The widest-unit fixture names every key the unit facts serializer writes.

Buffers for the facts document are sized against shared/UnitFactsWidest.h. The
native test finds every key of its UNIT_FACTS_KEYS in the widest documents;
this gate holds that list to the serializer's source, so a key added to the
serializer cannot stay out of the sizing.
"""
import re
from pathlib import Path

V2 = Path(__file__).resolve().parents[2]
SERIALIZER = (V2 / "shared/UnitHealth.h").read_text()
FIXTURE = (V2 / "shared/UnitFactsWidest.h").read_text()


def serializer_keys() -> set[str]:
    # The serializer is buildUnitHealthJson plus the per-block appenders it
    # calls, all between its append macro and its own end.
    start = SERIALIZER.index("#define UNIT_HEALTH_APPEND(")
    main = SERIALIZER.index("inline size_t buildUnitHealthJson(", start)
    end = SERIALIZER.index("\n}\n", main)
    appenders = re.findall(r"^inline size_t (unitHealthAppend\w+)\(", SERIALIZER[start:main], re.M)
    for name in appenders:
        assert f"o = {name}(buf, cap, o, u" in SERIALIZER[main:end], f"{name} is not called"
    return set(re.findall(r'\\"(\w+)\\":', SERIALIZER[start:end]))


def fixture_keys() -> list[str]:
    start = FIXTURE.index("UNIT_FACTS_KEYS[] = {")
    return re.findall(r'"(\w+)"', FIXTURE[start:FIXTURE.index("};", start)])


def test_the_serializer_is_found():
    assert {"width", "units", "age", "blf"} <= serializer_keys()


def test_the_fixture_lists_exactly_the_serializers_keys():
    listed = fixture_keys()
    assert len(listed) == len(set(listed)), "a key is listed twice"
    assert set(listed) == serializer_keys(), sorted(set(listed) ^ serializer_keys())
