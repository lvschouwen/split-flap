"""No logic in two firmware trees (epic #538).

Each test pins one piece of logic to its single home under
`firmware/v2/shared/` by forbidding the vocabulary a private re-implementation
would need. `tests/test_twiboot_shared.py` is the same idea for the twiboot
client.
"""

import re
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
V2 = REPO / "firmware/v2"
SHARED = V2 / "shared"
ROW_MASTERS = [V2 / "Master", V2 / "FollowerEsp01"]
TREES = ROW_MASTERS + [V2 / "Rescue"]
SKIP_DIRS = {".pio", "test", "tests", "managed_components", ".dummy"}
SOURCE_SUFFIXES = {".cpp", ".cc", ".h", ".hpp", ".ino", ".ini"}


def _strip_comments(src: str) -> str:
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.DOTALL)
    return re.sub(r"//[^\n]*", "", src)


def _sources(trees):
    for tree in trees:
        for path in sorted(tree.rglob("*")):
            if path.suffix not in SOURCE_SUFFIXES:
                continue
            if SKIP_DIRS & set(path.relative_to(tree).parts):
                continue
            yield path


def _offenders(pattern, trees=TREES):
    found = {}
    for path in _sources(trees):
        text = path.read_text()
        if path.suffix != ".ini":
            text = _strip_comments(text)
        hits = re.findall(pattern, text)
        if hits:
            found[str(path.relative_to(REPO))] = sorted(set(hits))
    return found


def test_the_gate_reads_the_row_master_sources():
    seen = set(_sources(TREES))
    assert V2 / "Master/DisplayTask.cpp" in seen
    assert V2 / "FollowerEsp01/FollowerBus.cpp" in seen
    assert V2 / "Master/platformio.ini" in seen


# --- #528: flap letter + speed mapping --------------------------------------

def test_no_tree_maps_characters_to_flaps_itself():
    """A private walk over the alphabet is how the ESP-01 row came to skip
    lowercase and unknown characters."""
    assert not _offenders(r"\bSFP_ALPHABET\b"), (
        "characters map to flap indices only in shared/FlapLetters.h")


def test_the_wire_speed_range_is_defined_once():
    assert not _offenders(r"(?:#define\s+|-D\s*)M(?:IN|AX)_SPEED\b"), (
        "MIN_SPEED/MAX_SPEED live in shared/FlapLetters.h")
    assert not _offenders(r"\bmap\s*\(\s*\w+\s*,\s*1\s*,\s*100\b"), (
        "the web speed conversion is convertSpeedToUnit()")


def test_the_follower_render_uses_the_shared_mapping():
    body = _strip_comments(
        (V2 / "FollowerEsp01/FollowerBus.cpp").read_text())
    assert "flapLetterOrBlank(" in body
    assert "convertSpeedToUnit(" in body


# --- #527: maintenance vocabulary + reflash plan ------------------------------

def test_no_tree_redefines_the_maintenance_vocabulary():
    """FollowerOps.h was a hand copy of three Master headers and missed the
    #405 protocol-mismatch clause and the #412 consecutive-failure halt."""
    owned = (r"\b(?:enum class (?:MaintOutcome|MaintReason|SelfTestOutcome|"
             r"ReflashState|OpResultState)|struct (?:MaintVerdict|SelfTestSlot|"
             r"ReflashProgress)|inline \w[\w\s\*]* (?:maintValidate\w+|"
             r"maintEncode\w+|maint\w+Name|reflash[A-Z]\w+|buildOpResultJson|"
             r"buildSelfTestJson|buildReflashJson|selfTestOutcomeName)\()")
    assert not _offenders(owned), (
        "op validators, result vocabulary and the reflash plan live in "
        "shared/MaintenancePolicy.h and shared/ReflashPlan.h")


def test_the_reflash_batch_size_is_a_build_flag_per_row_master():
    for tree, size in (("Master", "4"), ("FollowerEsp01", "2")):
        ini = (V2 / tree / "platformio.ini").read_text()
        flags = re.findall(r"-D\s*REFLASH_BATCH_SIZE=(\d+)", ini)
        assert flags == [size, size], f"{tree}: target and native env"
    assert not _offenders(r"#define\s+REFLASH_BATCH_SIZE\b")


def test_both_reflash_jobs_halt_on_consecutive_failures():
    for path in (V2 / "Master/DisplayTask.cpp",
                 V2 / "FollowerEsp01/FollowerBus.cpp"):
        body = _strip_comments(path.read_text())
        assert "reflashShouldHalt(" in body, path.name


# --- #529/#530: self-test wait, op grading, fact patches ---------------------

def test_no_tree_grades_an_op_outcome_by_hand():
    """The same op used to report wire-fail on one platform and
    postcondition-fail on the other."""
    hits = _offenders(r"\?\s*MaintOutcome::Ok\s*:\s*MaintOutcome::\w+",
                      ROW_MASTERS)
    assert not hits, f"grade through maintGrade*(): {hits}"


def test_no_tree_judges_self_test_replies_itself():
    assert not _offenders(r"\bSELFTEST_STATE_\w+|\bsawRunning\b|"
                          r"#define\s+SELF_TEST_\w+|"
                          r"\bconstexpr\s+\w+\s+SELF_TEST_\w+", ROW_MASTERS), (
        "the self-test wait is shared/SelfTestPoll.h")
    for path in ("Master/DisplayTask.cpp", "FollowerEsp01/FollowerWeb.cpp"):
        assert "selfTestPollObserve(" in (V2 / path).read_text(), path


def test_unit_read_validity_is_patched_through_the_shared_helpers():
    hits = _offenders(r"\b(?:offsetValid|odometerValid)\s*=(?!=)", ROW_MASTERS)
    # The bus files set them where they READ the value; nothing else may.
    readers = {"firmware/v2/Master/UnitBus.cpp",
               "firmware/v2/FollowerEsp01/FollowerBus.cpp"}
    assert set(hits) <= readers, hits
    for path in ("Master/DisplayIpc.h", "FollowerEsp01/FollowerBus.cpp"):
        assert "unitFactsInvalidateReads(" in (V2 / path).read_text(), path
