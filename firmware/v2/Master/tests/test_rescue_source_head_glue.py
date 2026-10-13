"""Source gate for the rescue source head (#613).

RescueSourceHead.h (../release) is natively tested. What a native test cannot
reach: that both trees stamp RESCUE_SOURCE_HEAD into BuildVersion.h through
the one helper, that the rescue image embeds it as the marker and reads its
own settings off that marker (so the linker keeps it), and that the master
scans the factory slot for it and judges by it, not by the repo rev.
"""
import re
from pathlib import Path

MASTER = Path(__file__).resolve().parent.parent
V2 = MASTER.parent


def _code(path):
    return re.sub(r"//[^\n]*", "", path.read_text())


def test_both_trees_stamp_the_head_through_fwbuild():
    tools = (V2 / "buildtools" / "fwbuild.py").read_text()
    assert "RESCUE_SOURCE_HEAD" in tools and "def rescue_source_head(" in tools
    for tree in ("Master", "Rescue"):
        assert "write_version_header(" in (V2 / tree / "build_assets.py").read_text(), tree


def test_the_rescue_image_embeds_and_reads_its_marker():
    main = _code(V2 / "Rescue" / "main.cpp")
    assert "RESCUE_SOURCE_MARKER_PREFIX RESCUE_SOURCE_HEAD RESCUE_SOURCE_MARKER_SUFFIX" in main
    web = _code(V2 / "Rescue" / "RescueWeb.cpp")
    assert "rescueSourceHeadOfMarker(RESCUE_SOURCE_MARKER)" in web
    assert 'sourceHead' in web


def test_the_master_scans_the_slot_and_judges_by_the_head():
    slot = _code(MASTER / "FactorySlot.cpp")
    assert "rescueSourceHeadFind(" in slot
    assert "rescueSlotFacts(true, valid, rec, shaMatches, head, RESCUE_SOURCE_HEAD)" in slot
    assert "GIT_REV)" not in slot[slot.index("static RescueSlotFacts computeRescueSlotFacts"):]
