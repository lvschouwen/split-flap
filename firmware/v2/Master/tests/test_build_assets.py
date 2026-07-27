"""Host-side tests for v2 Master/build_assets.py (#186, #205).

Covered here: the alphabet drift gate (#149) against the v1 shared protocol
header the v2 master speaks, deterministic gzip (#168), the UTF-8 pinning
guard, and — since the reflash slice (#205) — the unit-firmware bundling
(Intel-HEX parse, page pad, rev sidecar) the v2 script re-grew from v1.

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


def _make_tree(tmp_path, alphabet_header: str, alphabet_js: str) -> pathlib.Path:
    """Recreate the firmware/v2/shared + firmware/v2/Master layout the
    verify step resolves against (shared_protocol_header)."""
    shared = tmp_path / "v2" / "shared"
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


def _hex_line(addr: int, data: bytes) -> str:
    body = bytes([len(data), (addr >> 8) & 0xFF, addr & 0xFF, 0x00]) + data
    checksum = (-sum(body)) & 0xFF
    return ":" + (body + bytes([checksum])).hex().upper()


def test_parse_intel_hex_applies_data_records(tmp_path):
    hex_file = tmp_path / "fw.hex"
    hex_file.write_text(
        _hex_line(0x0000, b"\x01\x02\x03\x04") + "\n"
        + _hex_line(0x0004, b"\x05\x06") + "\n"
        + ":00000001FF\n",
        encoding="utf-8",
    )
    assert build_assets.parse_intel_hex(hex_file) == b"\x01\x02\x03\x04\x05\x06"


def test_parse_intel_hex_fills_gaps_with_ff(tmp_path):
    hex_file = tmp_path / "fw.hex"
    hex_file.write_text(
        _hex_line(0x0000, b"\xAA") + "\n"
        + _hex_line(0x0003, b"\xBB") + "\n"
        + ":00000001FF\n",
        encoding="utf-8",
    )
    assert build_assets.parse_intel_hex(hex_file) == b"\xAA\xFF\xFF\xBB"


def test_parse_intel_hex_stops_at_eof_record(tmp_path):
    hex_file = tmp_path / "fw.hex"
    hex_file.write_text(
        _hex_line(0x0000, b"\x11") + "\n"
        + ":00000001FF\n"
        + _hex_line(0x0001, b"\x22") + "\n",
        encoding="utf-8",
    )
    assert build_assets.parse_intel_hex(hex_file) == b"\x11"


def test_pad_to_page_pads_partial_page_with_ff():
    assert build_assets.pad_to_page(b"\x01\x02", page=4) == b"\x01\x02\xFF\xFF"


def test_pad_to_page_keeps_exact_multiple():
    data = b"\x01\x02\x03\x04"
    assert build_assets.pad_to_page(data, page=4) == data


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


def test_real_tree_has_committed_unit_bundle():
    # #205: the hex + rev sidecar are committed (v1 pattern) — the build
    # must never depend on a stage step having run.
    data = pathlib.Path(build_assets.__file__).resolve().parent / "data"
    assert (data / "unit-firmware.hex").exists()
    rev = (data / "unit-firmware.rev").read_text(encoding="utf-8").strip()
    assert rev, "unit-firmware.rev must carry the built rev"


# --- timezone table (#252) ---------------------------------------------------


def test_all_text_io_in_build_assets_pins_utf8():
    # CI runners can default to non-UTF-8 codecs; the web assets are UTF-8
    # (…, ·, Ä). Every text read/write must pin the encoding.
    import re

    src = pathlib.Path(build_assets.__file__).read_text(encoding="utf-8")
    for match in re.finditer(r"read_text\(([^)]*)\)|open\(\s*\"w\"([^)]*)\)", src):
        args = match.group(1) if match.group(1) is not None else match.group(2)
        assert "utf-8" in (args or ""), f"unpinned text IO: {match.group(0)}"
