"""Host-side tests for v2 Master/build_assets.py (#186, #205).

Covered here: deterministic gzip (#168), the UTF-8 pinning guard, the
timezone table, and the unit-bundle sidecars as this tree's script sees them.
The page's alphabet is generated from the protocol header (test_web_page.py). The
shared helpers (Intel-HEX parse, page pad, rev stamping) are tested in
firmware/v2/buildtools/tests.

Run with:
    pytest tests/
"""

from __future__ import annotations

import pathlib
import sys

import pytest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))

import build_assets  # noqa: E402


# --- unit-firmware bundling (#205) -----------------------------------------


def test_bundled_unit_rev_reads_sidecar(tmp_path):
    (tmp_path / "data").mkdir()
    (tmp_path / "data" / "unit-firmware.rev").write_text("0fd341f\n", encoding="utf-8")
    assert build_assets.bundled_unit_rev(tmp_path, fallback="deadbee") == "0fd341f"


def test_bundled_unit_rev_falls_back_when_missing(tmp_path):
    (tmp_path / "data").mkdir()
    assert build_assets.bundled_unit_rev(tmp_path, fallback="deadbee") == "deadbee"


def test_bundled_unit_rev_falls_back_on_empty_sidecar(tmp_path):
    (tmp_path / "data").mkdir()
    (tmp_path / "data" / "unit-firmware.rev").write_text(" \n", encoding="utf-8")
    assert build_assets.bundled_unit_rev(tmp_path, fallback="deadbee") == "deadbee"


# --- proven content-equivalent revs (#440) -----------------------------------

def test_bundled_unit_equivalent_revs_joins_the_sidecar(tmp_path):
    (tmp_path / "data").mkdir()
    (tmp_path / "data" / "unit-firmware.equiv").write_text(
        "d6e8a8a\naaaaaaa\n", encoding="utf-8")
    assert build_assets.bundled_unit_equivalent_revs(tmp_path) == \
        "d6e8a8a,aaaaaaa"


def test_bundled_unit_equivalent_revs_empty_without_sidecar(tmp_path):
    """No sidecar → bundle-rev equality only, the pre-#440 behaviour."""
    (tmp_path / "data").mkdir()
    assert build_assets.bundled_unit_equivalent_revs(tmp_path) == ""


def test_bundled_unit_equivalent_revs_empty_sidecar_yields_nothing(tmp_path):
    """stage writes the sidecar even when nothing survives — an empty file
    must bake an empty define, never a stray comma."""
    (tmp_path / "data").mkdir()
    (tmp_path / "data" / "unit-firmware.equiv").write_text("", encoding="utf-8")
    assert build_assets.bundled_unit_equivalent_revs(tmp_path) == ""


def test_bundled_unit_equivalent_revs_skips_blanks_and_comments(tmp_path):
    (tmp_path / "data").mkdir()
    (tmp_path / "data" / "unit-firmware.equiv").write_text(
        "# note\n\n  d6e8a8a  \n\n", encoding="utf-8")
    assert build_assets.bundled_unit_equivalent_revs(tmp_path) == "d6e8a8a"


def test_real_tree_has_committed_unit_bundle():
    # #205: the hex + rev sidecar are committed (v1 pattern) — the build
    # must never depend on a stage step having run.
    data = pathlib.Path(build_assets.__file__).resolve().parent / "data"
    assert (data / "unit-firmware.hex").exists()
    rev = (data / "unit-firmware.rev").read_text(encoding="utf-8").strip()
    assert rev, "unit-firmware.rev must carry the built rev"


# --- timezone table (#252) ---------------------------------------------------


def test_build_tz_json_maps_iana_to_posix(tmp_path):
    import json

    csv_file = tmp_path / "zones.csv"
    csv_file.write_text(
        '"Europe/Amsterdam","CET-1CEST,M3.5.0,M10.5.0/3"\n"Asia/Tokyo","JST-9"\n',
        encoding="utf-8",
    )
    table = json.loads(build_assets.build_tz_json(csv_file))
    assert table["Europe/Amsterdam"] == "CET-1CEST,M3.5.0,M10.5.0/3"
    assert table["Asia/Tokyo"] == "JST-9"


def test_build_tz_json_utc_head_entry_is_empty_default(tmp_path):
    # "" is the firmware's stored UTC default — the table's UTC entry must
    # round-trip to it, not to a POSIX "UTC0".
    import json

    csv_file = tmp_path / "zones.csv"
    csv_file.write_text('"Etc/UTC","UTC0"\n', encoding="utf-8")
    table = json.loads(build_assets.build_tz_json(csv_file))
    assert table["UTC"] == ""


def test_real_tree_has_vendored_zones_csv():
    import json

    data = pathlib.Path(build_assets.__file__).resolve().parent / "data"
    table = json.loads(build_assets.build_tz_json(data / "zones.csv"))
    assert len(table) > 400
    assert table["Europe/Amsterdam"] == "CET-1CEST,M3.5.0,M10.5.0/3"


# --- deterministic gzip (#168) ---------------------------------------------


def test_compress_asset_zero_mtime_and_deterministic():
    # The gzip MTIME header field (bytes 4..8, little-endian) must be zero so
    # two bakes of the same source are byte-identical — otherwise WebAssets.h,
    # the firmware bin and sketchMd5 differ on every rebuild.
    out = build_assets.compress_asset(b"hello split-flap")
    assert out[4:8] == b"\x00\x00\x00\x00"
    assert out == build_assets.compress_asset(b"hello split-flap")


def test_compress_asset_roundtrips():
    import gzip

    payload = b"<html>calibration</html>" * 50
    assert gzip.decompress(build_assets.compress_asset(payload)) == payload


# --- text IO encoding pinning ----------------------------------------------


def test_all_text_io_in_build_assets_pins_utf8():
    # CI runners can default to non-UTF-8 codecs; the web assets are UTF-8
    # (…, ·, Ä). Every text read/write must pin the encoding.
    import re

    src = pathlib.Path(build_assets.__file__).read_text(encoding="utf-8")
    for match in re.finditer(r"read_text\(([^)]*)\)|open\(\s*\"w\"([^)]*)\)", src):
        args = match.group(1) if match.group(1) is not None else match.group(2)
        assert "utf-8" in (args or ""), f"unpinned text IO: {match.group(0)}"
