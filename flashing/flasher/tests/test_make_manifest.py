import hashlib
import os
import re
import time

import pytest
from flasher.make_manifest import (GateError, build_manifest, consistency_gate,
                                   freshness_gate, git_rev)


def test_build_manifest_hashes_all_files(tmp_path):
    (tmp_path / "a.bin").write_bytes(b"aaa")
    (tmp_path / "sub").mkdir()
    (tmp_path / "sub" / "b.hex").write_bytes(b"bbb")
    (tmp_path / "manifest.json").write_text("{}")  # must be excluded
    m = build_manifest(tmp_path, "abc1234", "2026-07-05")
    assert m["git_rev"] == "abc1234"
    assert m["assets"]["a.bin"] == hashlib.sha256(b"aaa").hexdigest()
    assert m["assets"]["sub/b.hex"] == hashlib.sha256(b"bbb").hexdigest()
    assert "manifest.json" not in m["assets"]


def test_gate_passes_when_consistent(tmp_path):
    built_hex = tmp_path / "built.hex"; built_hex.write_bytes(b"HEX")
    staged_hex = tmp_path / "staged.hex"; staged_hex.write_bytes(b"HEX")
    built_rev = tmp_path / "built.rev"; built_rev.write_text("abc1234\n")
    staged_rev = tmp_path / "staged.rev"; staged_rev.write_text("abc1234\n")
    consistency_gate(built_hex, staged_hex, built_rev, staged_rev)  # no raise


def test_gate_rejects_stale_staged_hex(tmp_path):
    built_hex = tmp_path / "built.hex"; built_hex.write_bytes(b"NEW")
    staged_hex = tmp_path / "staged.hex"; staged_hex.write_bytes(b"OLD")
    built_rev = tmp_path / "built.rev"; built_rev.write_text("abc1234")
    staged_rev = tmp_path / "staged.rev"; staged_rev.write_text("abc1234")
    with pytest.raises(GateError, match="differs"):
        consistency_gate(built_hex, staged_hex, built_rev, staged_rev)


def test_gate_rejects_rev_mismatch(tmp_path):
    built_hex = tmp_path / "built.hex"; built_hex.write_bytes(b"HEX")
    staged_hex = tmp_path / "staged.hex"; staged_hex.write_bytes(b"HEX")
    built_rev = tmp_path / "built.rev"; built_rev.write_text("abc1234")
    staged_rev = tmp_path / "staged.rev"; staged_rev.write_text("dead999")
    with pytest.raises(GateError, match="rev") as exc_info:
        consistency_gate(built_hex, staged_hex, built_rev, staged_rev)
    assert "abc1234" in str(exc_info.value)
    assert "dead999" in str(exc_info.value)


def test_git_rev_matches_repo_rev_format():
    assert re.fullmatch(r"[0-9a-f]{7,12}(-dirty)?", git_rev())


def test_freshness_gate_rejects_master_built_before_staging(tmp_path):
    staged_hex = tmp_path / "unit-firmware.hex"; staged_hex.write_bytes(b"HEX")
    master_bin = tmp_path / "firmware.bin"; master_bin.write_bytes(b"BIN")
    now = time.time()
    os.utime(master_bin, (now - 100, now - 100))  # master built...
    os.utime(staged_hex, (now, now))               # ...before unit fw was staged
    with pytest.raises(GateError, match="OLDER"):
        freshness_gate(master_bin, staged_hex)


def test_freshness_gate_passes_when_master_built_after_staging(tmp_path):
    staged_hex = tmp_path / "unit-firmware.hex"; staged_hex.write_bytes(b"HEX")
    master_bin = tmp_path / "firmware.bin"; master_bin.write_bytes(b"BIN")
    now = time.time()
    os.utime(staged_hex, (now - 100, now - 100))  # staged first...
    os.utime(master_bin, (now, now))               # ...then master rebuilt
    freshness_gate(master_bin, staged_hex)  # no raise
