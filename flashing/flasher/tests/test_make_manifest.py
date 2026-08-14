import subprocess

import pytest
import flasher.make_manifest as mm
from flasher.make_manifest import (STAGE_DATA_DIRS, GateError, cmd_gate,
                                   stage_bundle, staged_rev_gate,
                                   unit_source_head)


def test_stage_data_dirs_target_v2_only():
    # #283: v1 ESPMaster is frozen — its committed bundle is a fossil of the
    # last pre-freeze stage. The live unit bundles are the v2 master's and
    # the ESP-01 follower's (#298).
    tails = {"/".join(p.parts[-3:]) for p in STAGE_DATA_DIRS}
    assert tails == {"v2/Master/data", "v2/FollowerEsp01/data"}


def test_stage_bundle_copies_into_every_tree(tmp_path):
    built_hex = tmp_path / "firmware.hex"; built_hex.write_bytes(b"HEX")
    built_rev = tmp_path / "firmware.rev"; built_rev.write_text("abc1234\n")
    trees = [tmp_path / "a", tmp_path / "b"]
    for t in trees:
        t.mkdir()
    stage_bundle(built_hex, built_rev, trees)
    for t in trees:
        assert (t / "unit-firmware.hex").read_bytes() == b"HEX"
        assert (t / "unit-firmware.rev").read_text() == "abc1234\n"


# --- rev drift gate ----------------------------------------------------------
# The Unit binary embeds GIT_REV (build_version.py), so a fresh build can
# never byte-match the committed bundle. The gate therefore works on revs:
# the staged .rev must already contain the last commit touching Unit sources.

def _git(repo, *args) -> str:
    return subprocess.run(["git", *args], cwd=repo, check=True,
                          capture_output=True, text=True).stdout.strip()


def _commit(repo, relpath: str, content: str, msg: str) -> str:
    p = repo / relpath
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(content)
    _git(repo, "add", "-A")
    _git(repo, "commit", "-qm", msg)
    return _git(repo, "rev-parse", "--short", "HEAD")


@pytest.fixture
def repo(tmp_path):
    _git(tmp_path, "init", "-q")
    _git(tmp_path, "config", "user.email", "t@test")
    _git(tmp_path, "config", "user.name", "t")
    _commit(tmp_path, "firmware/v2/Unit/Unit.ino", "v1", "unit code")
    return tmp_path


def test_rev_gate_passes_when_staged_at_unit_head(repo):
    staged = _git(repo, "rev-parse", "--short", "HEAD")
    staged_rev_gate(staged, repo=repo)  # no raise


def test_rev_gate_requires_the_exact_unit_source_head(repo):
    # #440 tightened this from "contains the Unit change" to "IS the Unit
    # source head". The sidecar and the rev baked into the unit binary are
    # both stamped by build_version.py from unit_source_head(), so anything
    # else means the two sides disagree — and every unit would then read
    # OUTDATED against its own bundle.
    staged = _commit(repo, "firmware/v2/Master/data/unit-firmware.rev",
                     "x", "artifact commit")
    with pytest.raises(GateError, match="source head"):
        staged_rev_gate(staged, repo=repo)


def test_rev_gate_rejects_unit_change_after_staging(repo):
    staged = _git(repo, "rev-parse", "--short", "HEAD")
    _commit(repo, "firmware/v2/Unit/Unit.ino", "v2", "unit code moved on")
    with pytest.raises(GateError, match="changed"):
        staged_rev_gate(staged, repo=repo)


def test_rev_gate_rejects_shared_header_change_after_staging(repo):
    # ../shared/SplitFlapProtocol.h compiles into the Unit binary too.
    staged = _git(repo, "rev-parse", "--short", "HEAD")
    _commit(repo, "firmware/v2/shared/SplitFlapProtocol.h", "v2", "protocol")
    with pytest.raises(GateError, match="changed"):
        staged_rev_gate(staged, repo=repo)


def test_rev_gate_ignores_unit_test_only_changes(repo):
    # Host-side test changes don't alter the shipped binary — no re-stage.
    staged = _git(repo, "rev-parse", "--short", "HEAD")
    _commit(repo, "firmware/v2/Unit/test/test_main.cpp", "t", "tests only")
    staged_rev_gate(staged, repo=repo)  # no raise


def test_rev_gate_rejects_dirty_staged_rev(repo):
    staged = _git(repo, "rev-parse", "--short", "HEAD")
    with pytest.raises(GateError, match="dirty"):
        staged_rev_gate(f"{staged}-dirty", repo=repo)


def test_rev_gate_rejects_unknown_staged_rev(repo):
    with pytest.raises(GateError, match="not a commit"):
        staged_rev_gate("dead999", repo=repo)


def test_rev_gate_rejects_empty_staged_rev(repo):
    with pytest.raises(GateError, match="empty"):
        staged_rev_gate("", repo=repo)


def test_unit_source_head_tracks_unit_and_shared(repo):
    first = _git(repo, "rev-parse", "HEAD")
    assert unit_source_head(repo) == first
    _commit(repo, "docs/other.md", "x", "unrelated")
    assert unit_source_head(repo) == first
    _commit(repo, "firmware/v2/shared/SplitFlapProtocol.h", "p", "protocol")
    assert unit_source_head(repo) == _git(repo, "rev-parse", "HEAD")


# --- proven content-equivalent revs (#440) -----------------------------------
# A unit reports the COMMIT it was built at, not the CODE it runs, so a
# comment-only edit re-labels every unit OUTDATED. equivalent-revs.txt records
# revs measured to run byte-identical machine code, paired with the CONTENT
# HASH of that image. Anchoring on the artifact (not a commit) is what lets an
# entry survive commits that cannot touch the Nano binary while evaporating
# the instant one that can does.

HASH_A = "a" * 64
HASH_B = "b" * 64


def _equiv_file(repo, text: str):
    p = repo / "firmware/v2/Unit/equivalent-revs.txt"
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(text)
    return p


def test_equivalents_survive_for_the_image_they_were_proven_against(repo):
    _equiv_file(repo, f"# comment\n\nd6e8a8a {HASH_A}\n")
    assert mm.load_equivalent_revs(HASH_A, repo=repo) == ["d6e8a8a"]


def test_equivalents_are_dropped_once_the_image_changes(repo):
    _equiv_file(repo, f"d6e8a8a {HASH_A}\n")
    # A REAL Unit change lands: the built image hashes differently, so the
    # proof no longer applies and the entry evaporates with no one editing it.
    assert mm.load_equivalent_revs(HASH_B, repo=repo) == []


def test_equivalents_ignore_comments_blanks_and_malformed_lines(repo):
    _equiv_file(repo, f"""
        # a comment
        d6e8a8a {HASH_A}
        onlyonefield
        aaaaaaa {HASH_A} trailing junk
        bbbbbbb {HASH_B}
        ccccccc deadbee
    """.replace("        ", ""))
    # Only the well-formed line proven against THIS image survives: the
    # 3-field line is malformed, HASH_B is a different image, and "deadbee"
    # is too short to be a content hash.
    assert mm.load_equivalent_revs(HASH_A, repo=repo) == ["d6e8a8a"]


def test_equivalents_absent_file_is_not_an_error(repo):
    assert mm.load_equivalent_revs(HASH_A, repo=repo) == []


def test_empty_content_hash_never_matches(repo):
    """A hash we failed to compute must not silently match a blank field."""
    _equiv_file(repo, "d6e8a8a \n")
    assert mm.load_equivalent_revs("", repo=repo) == []


# --- image content hashing ---------------------------------------------------

def _write_hex(path, payload: bytes):
    """Minimal Intel HEX: one data record at 0x0000 plus EOF."""
    body = bytes([len(payload), 0x00, 0x00, 0x00]) + payload
    checksum = (-sum(body)) & 0xFF
    path.write_text(":" + (body + bytes([checksum])).hex().upper() +
                    "\n:00000001FF\n")


def test_content_hash_ignores_the_embedded_rev(tmp_path):
    """The whole premise: two images differing only in the baked rev string
    are the same code and must hash identically."""
    a = tmp_path / "a.hex"; _write_hex(a, b"\x01\x02d6e8a8a\x03")
    b = tmp_path / "b.hex"; _write_hex(b, b"\x01\x02c0729fe\x03")
    assert mm.image_content_hash(a, "d6e8a8a") == \
           mm.image_content_hash(b, "c0729fe")


def test_content_hash_still_sees_a_real_code_change(tmp_path):
    a = tmp_path / "a.hex"; _write_hex(a, b"\x01\x02d6e8a8a\x03")
    b = tmp_path / "b.hex"; _write_hex(b, b"\x01\x99c0729fe\x03")
    assert mm.image_content_hash(a, "d6e8a8a") != \
           mm.image_content_hash(b, "c0729fe")


def test_stage_writes_the_equiv_sidecar(tmp_path):
    built_hex = tmp_path / "firmware.hex"; built_hex.write_bytes(b"HEX")
    built_rev = tmp_path / "firmware.rev"; built_rev.write_text("abc1234\n")
    tree = tmp_path / "a"; tree.mkdir()
    stage_bundle(built_hex, built_rev, [tree], equivalents=["d6e8a8a", "eee"])
    assert (tree / "unit-firmware.equiv").read_text() == "d6e8a8a\neee\n"


def test_stage_overwrites_a_stale_equiv_sidecar(tmp_path):
    """The sidecar must always describe THIS stage — a leftover from a
    previous one would silently keep suppressing a real OUTDATED."""
    built_hex = tmp_path / "firmware.hex"; built_hex.write_bytes(b"HEX")
    built_rev = tmp_path / "firmware.rev"; built_rev.write_text("abc1234\n")
    tree = tmp_path / "a"; tree.mkdir()
    (tree / "unit-firmware.equiv").write_text("stale000\n")
    stage_bundle(built_hex, built_rev, [tree], equivalents=[])
    assert (tree / "unit-firmware.equiv").read_text() == ""


def test_unit_src_pathspecs_match_the_build_stamp():
    """build_version.py stamps the binary and make_manifest.py gates the
    sidecar; both decide what "the Unit sources" are. If the two lists
    diverge, the baked rev and the staged rev disagree and EVERY unit reads
    OUTDATED against its own bundle. build_version.py cannot import from here
    (it runs inside a PlatformIO build), so pin them instead."""
    import importlib.util

    path = mm.REPO / "firmware/v2/Unit/build_version.py"
    spec = importlib.util.spec_from_file_location("unit_build_version", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    assert mod.UNIT_SRC_PATHSPECS == mm.UNIT_SRC_PATHSPECS


def test_cmd_gate_rejects_missing_rev_sidecar(repo, monkeypatch):
    data = repo / "firmware/v2/Master/data"
    data.mkdir(parents=True)
    monkeypatch.setattr(mm, "REPO", repo)
    monkeypatch.setattr(mm, "STAGE_DATA_DIRS", [data])
    with pytest.raises(GateError, match="missing"):
        cmd_gate()


def test_cmd_gate_reads_staged_rev_from_v2_tree(repo, monkeypatch, capsys):
    staged = _git(repo, "rev-parse", "--short", "HEAD")
    data = repo / "firmware/v2/Master/data"
    data.mkdir(parents=True)
    (data / "unit-firmware.rev").write_text(f"{staged}\n")
    monkeypatch.setattr(mm, "REPO", repo)
    monkeypatch.setattr(mm, "STAGE_DATA_DIRS", [data])
    cmd_gate()  # no raise
    assert "gate passed" in capsys.readouterr().out
