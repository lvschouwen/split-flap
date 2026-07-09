"""Host-side tests for v2 Master/build_assets.py (#186).

The v2 script is the v1 pipeline minus the unit-firmware bundling, so the
Intel-HEX/pad_to_page tests stay v1-only. What is covered here: the alphabet
drift gate (#149) against the v1 shared protocol header the v2 master speaks,
deterministic gzip (#168), and the UTF-8 pinning guard.

Run with:
    pytest tests/
"""

from __future__ import annotations

import pathlib
import sys

import pytest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))

import build_assets  # noqa: E402


# --- alphabet drift check (#149) ------------------------------------------

EXPECTED_ALPHABET = " ABCDEFGHIJKLMNOPQRSTUVWXYZ$&#0123456789:.-?!"


def test_parse_header_alphabet_extracts_literal():
    header = f'#define SFP_ALPHABET "{EXPECTED_ALPHABET}"\n'
    assert build_assets.parse_header_alphabet(header) == EXPECTED_ALPHABET


def test_parse_header_alphabet_raises_when_missing():
    with pytest.raises(ValueError):
        build_assets.parse_header_alphabet("#define SOMETHING_ELSE 1\n")


def test_parse_js_calibration_letters_joins_chars():
    js = "const CALIBRATION_LETTERS = [' ','A','B','$','&','#','?','!'];"
    assert build_assets.parse_js_calibration_letters(js) == " AB$&#?!"


def test_parse_js_calibration_letters_raises_when_missing():
    with pytest.raises(ValueError):
        build_assets.parse_js_calibration_letters("const OTHER = [1,2,3];")


def _make_tree(tmp_path, alphabet_header: str, alphabet_js: str) -> pathlib.Path:
    """Recreate the firmware/v1/shared + firmware/v2/Master layout the
    verify step resolves against (shared_protocol_header)."""
    shared = tmp_path / "v1" / "shared"
    shared.mkdir(parents=True)
    (shared / "SplitFlapProtocol.h").write_text(
        f'#define SFP_ALPHABET "{alphabet_header}"\n', encoding="utf-8"
    )
    project = tmp_path / "v2" / "Master"
    (project / "data").mkdir(parents=True)
    js_array = ",".join(f"'{c}'" for c in alphabet_js)
    (project / "data" / "script.js").write_text(
        f"const CALIBRATION_LETTERS = [{js_array}];\n", encoding="utf-8"
    )
    return project


def test_shared_protocol_header_points_into_v1_tree(tmp_path):
    project = tmp_path / "v2" / "Master"
    header = build_assets.shared_protocol_header(project)
    assert header == tmp_path / "v1" / "shared" / "SplitFlapProtocol.h"


def test_verify_js_alphabet_passes_on_match(tmp_path):
    project = _make_tree(tmp_path, EXPECTED_ALPHABET, EXPECTED_ALPHABET)
    build_assets.verify_js_alphabet(project)  # must not raise


def test_verify_js_alphabet_fails_on_drift(tmp_path):
    # Drop the trailing '!' so the JS drifts from the header.
    project = _make_tree(tmp_path, EXPECTED_ALPHABET, EXPECTED_ALPHABET[:-1])
    with pytest.raises(ValueError, match="drift"):
        build_assets.verify_js_alphabet(project)


def test_real_tree_alphabet_is_in_sync():
    # The v2 data/ is a copy of v1's UI and both masters speak the same
    # protocol header — run the actual gate against the working tree so a
    # drifted copy fails in pytest before it fails the firmware build.
    project = pathlib.Path(build_assets.__file__).resolve().parent
    build_assets.verify_js_alphabet(project)


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
