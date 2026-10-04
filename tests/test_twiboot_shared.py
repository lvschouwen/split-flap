"""One twiboot client (#526).

The twiboot I2C flash protocol lives once, in `firmware/v2/shared/
TwibootFlash.h`; each row master only supplies a Wire adapter. Two private
implementations drifted before — fixes landed in one tree and not the other —
so no tree may speak the protocol by hand again, and both flash paths must
keep the shared guards.
"""

import re
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parent.parent
V2 = REPO / "firmware/v2"
TREES = [V2 / "Master", V2 / "FollowerEsp01", V2 / "Rescue"]
SKIP_DIRS = {".pio", "test", "tests", "managed_components", ".dummy"}

# Raw protocol vocabulary: using any of it means framing a twiboot command.
WIRE_TOKEN = re.compile(r"\bTWIBOOT_(?:CMD|MEMTYPE|BOOTTYPE)_\w+")

FLASH_PATHS = {
    "Master": (V2 / "Master/UnitBus.cpp", "UnitFlashResult unitBusFlashUnit("),
    "ESP-01": (V2 / "FollowerEsp01/FollowerBus.cpp",
               "static bool flashUnitSteps("),
}


def _strip_comments(src: str) -> str:
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.DOTALL)
    return re.sub(r"//[^\n]*", "", src)


def _function_body(src: str, signature: str) -> str:
    start = src.index(signature)
    depth = 0
    for i in range(src.index("{", start), len(src)):
        if src[i] == "{":
            depth += 1
        elif src[i] == "}":
            depth -= 1
            if depth == 0:
                return src[start:i + 1]
    raise AssertionError(f"unterminated body: {signature}")


# The constants and the one client that may use them.
PROTOCOL_HOME = {"TwibootProtocol.h", "TwibootFlash.h"}


def _tree_sources():
    for tree in TREES:
        for path in sorted(tree.rglob("*")):
            if path.suffix not in {".cpp", ".cc", ".h", ".hpp", ".ino"}:
                continue
            if SKIP_DIRS & set(path.relative_to(tree).parts):
                continue
            yield path
    for path in sorted((V2 / "shared").glob("*.h")):
        if path.name not in PROTOCOL_HOME:
            yield path


def test_the_gate_sees_the_bus_files():
    seen = set(_tree_sources())
    for path, _ in FLASH_PATHS.values():
        assert path in seen


def test_no_tree_frames_a_twiboot_command_itself():
    offenders = {}
    for path in _tree_sources():
        hits = WIRE_TOKEN.findall(_strip_comments(path.read_text()))
        if hits:
            offenders[str(path.relative_to(REPO))] = sorted(set(hits))
    assert not offenders, (
        "twiboot commands are framed only in shared/TwibootFlash.h: "
        f"{offenders}")


@pytest.mark.parametrize("tree", sorted(FLASH_PATHS))
def test_flash_path_keeps_the_shared_guards(tree):
    path, signature = FLASH_PATHS[tree]
    body = _function_body(_strip_comments(path.read_text()), signature)
    guard = body.index("twibootImageFits(")
    live = body.index("twibootAwaitBootloader(")
    chip = body.index("twibootVerifyChip(")
    page = body.index("twibootFlashAndVerifyPage(")
    leave = body.index("twibootExit(")
    sketch = body.index("twibootAwaitSketch(")
    reboot = body.index("SFP_CMD_REBOOT") if "SFP_CMD_REBOOT" in body \
        else body.index("rebootUnit(")
    assert guard < live < chip < page < leave < sketch < reboot, (
        f"{tree}: size guard, liveness, chip check, pages, exit, sketch "
        "wait, reboot — in that order")
