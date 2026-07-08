"""Host-side tests for ESPMaster/build_assets.py.

The build script bundles the web UI and unit firmware into PROGMEM arrays
at PIO build time. Two pure-logic helpers are exercised here:

  parse_intel_hex(path) -> bytes
  pad_to_page(data, page=128) -> bytes

Run with:
    pytest ESPMaster/tests/
"""

from __future__ import annotations

import pathlib
import sys

import pytest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))

import build_assets  # noqa: E402


# --- parse_intel_hex ------------------------------------------------------


def test_parse_intel_hex_single_data_record(tmp_path):
    # Data: 02 bytes at offset 0x0000 = 0xDE 0xAD, then EOF.
    hex_path = tmp_path / "tiny.hex"
    hex_path.write_text(":02000000DEAD73\n:00000001FF\n")

    result = build_assets.parse_intel_hex(hex_path)
    assert result == b"\xDE\xAD"


def test_parse_intel_hex_contiguous_records(tmp_path):
    # Two contiguous 4-byte records at 0x0000 and 0x0004.
    hex_path = tmp_path / "two.hex"
    hex_path.write_text(
        ":0400000001020304F2\n"
        ":0400040005060708DE\n"
        ":00000001FF\n"
    )

    result = build_assets.parse_intel_hex(hex_path)
    assert result == bytes(range(1, 9))


def test_parse_intel_hex_fills_gap_with_0xFF(tmp_path):
    # Data at 0x0000..0x0001, then at 0x0004..0x0005; gap is 0xFF.
    hex_path = tmp_path / "gap.hex"
    hex_path.write_text(
        ":020000000102FB\n"
        ":020004000708EB\n"
        ":00000001FF\n"
    )

    result = build_assets.parse_intel_hex(hex_path)
    assert result == b"\x01\x02\xFF\xFF\x07\x08"


def test_parse_intel_hex_stops_at_eof_record(tmp_path):
    # Bytes after the EOF record must be ignored.
    hex_path = tmp_path / "eof.hex"
    hex_path.write_text(
        ":01000000AAFF\n"
        ":00000001FF\n"
        ":01000100BBFE\n"  # this should NOT be picked up
    )

    result = build_assets.parse_intel_hex(hex_path)
    assert result == b"\xAA"


def test_parse_intel_hex_ignores_non_colon_lines(tmp_path):
    hex_path = tmp_path / "comments.hex"
    hex_path.write_text(
        "; a comment\n"
        "\n"
        ":01000000AAFF\n"
        "; trailing noise\n"
        ":00000001FF\n"
    )

    result = build_assets.parse_intel_hex(hex_path)
    assert result == b"\xAA"


# --- pad_to_page ----------------------------------------------------------


def test_pad_to_page_noop_when_already_aligned():
    data = b"\x00" * 256
    assert build_assets.pad_to_page(data, page=128) == data


def test_pad_to_page_pads_to_boundary_with_0xFF():
    data = b"\xAA" * 130  # 128 + 2
    padded = build_assets.pad_to_page(data, page=128)
    assert len(padded) == 256
    assert padded[:130] == data
    assert padded[130:] == b"\xFF" * 126


def test_pad_to_page_empty_stays_empty():
    # 0 % 128 == 0 -> the function returns input unchanged.
    assert build_assets.pad_to_page(b"", page=128) == b""


def test_pad_to_page_default_page_size_is_128():
    assert len(build_assets.pad_to_page(b"\x00" * 10)) == 128


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


def test_verify_js_alphabet_passes_on_match(tmp_path):
    shared = tmp_path / "shared"
    shared.mkdir()
    (shared / "SplitFlapProtocol.h").write_text(
        f'#define SFP_ALPHABET "{EXPECTED_ALPHABET}"\n'
    )
    project = tmp_path / "ESPMaster"
    (project / "data").mkdir(parents=True)
    js_array = ",".join(f"'{c}'" for c in EXPECTED_ALPHABET)
    (project / "data" / "script.js").write_text(
        f"const CALIBRATION_LETTERS = [{js_array}];\n"
    )
    build_assets.verify_js_alphabet(project)  # must not raise


def test_verify_js_alphabet_fails_on_drift(tmp_path):
    shared = tmp_path / "shared"
    shared.mkdir()
    (shared / "SplitFlapProtocol.h").write_text(
        f'#define SFP_ALPHABET "{EXPECTED_ALPHABET}"\n'
    )
    project = tmp_path / "ESPMaster"
    (project / "data").mkdir(parents=True)
    # Drop the trailing '!' so the JS drifts from the header.
    js_array = ",".join(f"'{c}'" for c in EXPECTED_ALPHABET[:-1])
    (project / "data" / "script.js").write_text(
        f"const CALIBRATION_LETTERS = [{js_array}];\n"
    )
    with pytest.raises(ValueError, match="drift"):
        build_assets.verify_js_alphabet(project)
