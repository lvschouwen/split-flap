import hashlib
import os
import re
import time

import pytest
from flasher.make_manifest import (STAGE_DATA_DIRS, GateError, build_manifest,
                                   consistency_gate, freshness_gate, git_rev,
                                   optional_freshness_gate, stage_bundle)


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


def test_stage_data_dirs_cover_both_masters():
    # #205: v1 ESPMaster and v2 Master embed the SAME unit bundle; staging
    # into only one tree lets the other auto-push stale unit firmware.
    tails = {"/".join(p.parts[-3:]) for p in STAGE_DATA_DIRS}
    assert "v1/ESPMaster/data" in tails
    assert "v2/Master/data" in tails


def test_stage_bundle_copies_into_every_tree(tmp_path):
    built_hex = tmp_path / "firmware.hex"; built_hex.write_bytes(b"HEX")
    built_rev = tmp_path / "firmware.rev"; built_rev.write_text("abc1234\n")
    trees = [tmp_path / "v1data", tmp_path / "v2data"]
    for t in trees:
        t.mkdir()
    stage_bundle(built_hex, built_rev, trees)
    for t in trees:
        assert (t / "unit-firmware.hex").read_bytes() == b"HEX"
        assert (t / "unit-firmware.rev").read_text() == "abc1234\n"


def test_optional_freshness_gate_skips_missing_bin(tmp_path):
    # The v2 master bin is not part of the v1 flasher's collect flow — a dev
    # machine that never built v2 must still be able to collect.
    staged_hex = tmp_path / "unit-firmware.hex"; staged_hex.write_bytes(b"HEX")
    optional_freshness_gate(tmp_path / "missing.bin", staged_hex)  # no raise


def test_optional_freshness_gate_rejects_stale_existing_bin(tmp_path):
    staged_hex = tmp_path / "unit-firmware.hex"; staged_hex.write_bytes(b"HEX")
    master_bin = tmp_path / "firmware.bin"; master_bin.write_bytes(b"BIN")
    now = time.time()
    os.utime(master_bin, (now - 100, now - 100))
    os.utime(staged_hex, (now, now))
    with pytest.raises(GateError, match="OLDER"):
        optional_freshness_gate(master_bin, staged_hex)


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
