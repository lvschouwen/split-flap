import hashlib
import json
import pytest
from flasher.assets import ManifestError, validate_manifest, REQUIRED_ASSETS


def make_assets(tmp_path, names):
    manifest = {"git_rev": "abc1234", "build_date": "2026-07-05", "assets": {}}
    for name in names:
        p = tmp_path / name
        p.write_bytes(b"data-" + name.encode())
        manifest["assets"][name] = hashlib.sha256(p.read_bytes()).hexdigest()
    return manifest


def test_valid_manifest_passes(tmp_path):
    manifest = make_assets(tmp_path, REQUIRED_ASSETS)
    validate_manifest(manifest, tmp_path)  # no raise


def test_missing_required_asset_rejected(tmp_path):
    manifest = make_assets(tmp_path, REQUIRED_ASSETS[1:])
    with pytest.raises(ManifestError, match=REQUIRED_ASSETS[0]):
        validate_manifest(manifest, tmp_path)


def test_hash_mismatch_rejected(tmp_path):
    manifest = make_assets(tmp_path, REQUIRED_ASSETS)
    (tmp_path / REQUIRED_ASSETS[0]).write_bytes(b"tampered")
    with pytest.raises(ManifestError, match="hash mismatch"):
        validate_manifest(manifest, tmp_path)


def test_listed_file_absent_rejected(tmp_path):
    manifest = make_assets(tmp_path, REQUIRED_ASSETS)
    manifest["assets"]["ghost.bin"] = "0" * 64
    with pytest.raises(ManifestError, match="ghost.bin"):
        validate_manifest(manifest, tmp_path)


def test_missing_keys_rejected(tmp_path):
    with pytest.raises(ManifestError, match="git_rev"):
        validate_manifest({"assets": {}}, tmp_path)
