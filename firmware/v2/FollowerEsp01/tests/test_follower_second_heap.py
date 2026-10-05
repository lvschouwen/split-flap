"""Source gate for the second heap (#564).

Which heap a buffer comes from is glue no native test can reach. Pin it: the
image is built with the second heap, one file selects it, and the three large
buffers are taken through that file.
"""
import re
from pathlib import Path

TREE = Path(__file__).resolve().parents[1]
SOURCES = [p for p in TREE.glob("*") if p.suffix in (".cpp", ".h")]


def _code(name):
    return (TREE / name).read_text()


def test_the_image_is_built_with_the_second_heap():
    ini = _code("platformio.ini")
    follower = ini.split("[env:follower]")[1].split("[env:")[0]
    flags = [line.strip() for line in follower.splitlines()]
    assert "-DPIO_FRAMEWORK_ARDUINO_MMU_CACHE16_IRAM48_SECHEAP_SHARED" in flags


def test_one_file_selects_the_second_heap():
    users = sorted(p.name for p in SOURCES if re.search(r"\bHeapSelect\w*\s+\w+;", p.read_text()))
    assert users == ["FollowerMem.cpp"]


def test_the_large_buffers_come_from_it():
    assert "unitsDoc = (char*)followerBufAlloc(cap);" in _code("FollowerLink.cpp")
    assert "bootDumpBytes = (uint8_t*)followerBufAlloc(BOOT_SECTION_LEN);" in _code("FollowerUnitJobs.cpp")
    assert "followerBufAlloc(sizeof(FollowerLogRing))" in _code("FollowerLog.cpp")
    # The ring is no longer part of the fixed memory.
    assert not re.search(r"static\s+FollowerLogRing\s+\w+;", _code("FollowerLog.cpp"))


def test_a_buffer_goes_back_through_the_same_file():
    for name in ("FollowerLink.cpp", "FollowerUnitJobs.cpp"):
        code = _code(name)
        assert "delete[] unitsDoc" not in code and "delete[] bootDumpBytes" not in code
    assert "followerBufFree(unitsDoc);" in _code("FollowerLink.cpp")
    assert "followerBufFree(bootDumpBytes);" in _code("FollowerUnitJobs.cpp")
