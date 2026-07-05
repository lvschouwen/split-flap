import hashlib
import pytest
from flasher.make_manifest import GateError, build_manifest, consistency_gate


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
    built = tmp_path / "built.hex"; built.write_bytes(b"HEX")
    staged = tmp_path / "staged.hex"; staged.write_bytes(b"HEX")
    rev = tmp_path / "unit-firmware.rev"; rev.write_text("abc1234\n")
    consistency_gate(built, staged, rev, "abc1234")  # no raise


def test_gate_rejects_stale_staged_hex(tmp_path):
    built = tmp_path / "built.hex"; built.write_bytes(b"NEW")
    staged = tmp_path / "staged.hex"; staged.write_bytes(b"OLD")
    rev = tmp_path / "r"; rev.write_text("abc1234")
    with pytest.raises(GateError, match="differs"):
        consistency_gate(built, staged, rev, "abc1234")


def test_gate_rejects_rev_mismatch(tmp_path):
    built = tmp_path / "built.hex"; built.write_bytes(b"HEX")
    staged = tmp_path / "staged.hex"; staged.write_bytes(b"HEX")
    rev = tmp_path / "r"; rev.write_text("dead999")
    with pytest.raises(GateError, match="rev"):
        consistency_gate(built, staged, rev, "abc1234")
