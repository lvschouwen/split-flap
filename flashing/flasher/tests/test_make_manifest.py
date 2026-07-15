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
    _commit(tmp_path, "firmware/v1/Unit/Unit.ino", "v1", "unit code")
    return tmp_path


def test_rev_gate_passes_when_staged_at_unit_head(repo):
    staged = _git(repo, "rev-parse", "--short", "HEAD")
    staged_rev_gate(staged, repo=repo)  # no raise


def test_rev_gate_passes_when_staged_after_unit_head(repo):
    # Bundle staged from a later tree that already contains the Unit change.
    staged = _commit(repo, "firmware/v2/Master/data/unit-firmware.rev",
                     "x", "artifact commit")
    staged_rev_gate(staged, repo=repo)  # no raise


def test_rev_gate_rejects_unit_change_after_staging(repo):
    staged = _git(repo, "rev-parse", "--short", "HEAD")
    _commit(repo, "firmware/v1/Unit/Unit.ino", "v2", "unit code moved on")
    with pytest.raises(GateError, match="changed"):
        staged_rev_gate(staged, repo=repo)


def test_rev_gate_rejects_shared_header_change_after_staging(repo):
    # ../shared/SplitFlapProtocol.h compiles into the Unit binary too.
    staged = _git(repo, "rev-parse", "--short", "HEAD")
    _commit(repo, "firmware/v1/shared/SplitFlapProtocol.h", "v2", "protocol")
    with pytest.raises(GateError, match="changed"):
        staged_rev_gate(staged, repo=repo)


def test_rev_gate_ignores_unit_test_only_changes(repo):
    # Host-side test changes don't alter the shipped binary — no re-stage.
    staged = _git(repo, "rev-parse", "--short", "HEAD")
    _commit(repo, "firmware/v1/Unit/test/test_main.cpp", "t", "tests only")
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
    _commit(repo, "firmware/v1/shared/SplitFlapProtocol.h", "p", "protocol")
    assert unit_source_head(repo) == _git(repo, "rev-parse", "HEAD")


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
