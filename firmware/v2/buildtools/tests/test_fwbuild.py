"""Tests for the shared build helpers (#535).
Run from firmware/v2/buildtools: python -m pytest tests/
"""

import gzip
import io
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))

import fwbuild  # noqa: E402


def _record(addr: int, rtype: int, data: bytes) -> str:
    body = bytes([len(data), (addr >> 8) & 0xFF, addr & 0xFF, rtype]) + data
    return ":" + (body + bytes([(-sum(body)) & 0xFF])).hex().upper()


def _hex_line(addr: int, data: bytes) -> str:
    return _record(addr, 0x00, data)


EOF_RECORD = ":00000001FF"


def _write(tmp_path, *lines):
    path = tmp_path / "fw.hex"
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return path


def test_image_applies_data_records(tmp_path):
    path = _write(tmp_path, _hex_line(0, b"\x01\x02\x03\x04"),
                  _hex_line(4, b"\x05\x06"), EOF_RECORD)
    assert fwbuild.ihex_to_image(path) == b"\x01\x02\x03\x04\x05\x06"


def test_image_fills_gaps_with_ff(tmp_path):
    path = _write(tmp_path, _hex_line(0, b"\xAA"), _hex_line(3, b"\xBB"),
                  EOF_RECORD)
    assert fwbuild.ihex_to_image(path) == b"\xAA\xFF\xFF\xBB"


def test_image_stops_at_eof_record(tmp_path):
    path = _write(tmp_path, _hex_line(0, b"\x11"), EOF_RECORD,
                  _hex_line(1, b"\x22"))
    assert fwbuild.ihex_to_image(path) == b"\x11"


def test_extended_linear_address_moves_the_base(tmp_path):
    """The image a master embeds and the image the bundle gate hashes used to
    come from two parsers, one of which ignored these records."""
    path = _write(tmp_path, _hex_line(0, b"\x01"),
                  _record(0, 0x04, b"\x00\x01"), _hex_line(2, b"\x02"),
                  EOF_RECORD)
    mem = fwbuild.ihex_to_memory(path)
    assert mem == {0: 0x01, 0x10002: 0x02}


def test_extended_segment_address_moves_the_base(tmp_path):
    path = _write(tmp_path, _record(0, 0x02, b"\x10\x00"),
                  _hex_line(1, b"\x07"), EOF_RECORD)
    assert fwbuild.ihex_to_memory(path) == {0x10001: 0x07}


def test_empty_hex_is_an_empty_image(tmp_path):
    assert fwbuild.ihex_to_image(_write(tmp_path, EOF_RECORD)) == b""


def test_pad_to_page_pads_partial_page_with_ff():
    assert fwbuild.pad_to_page(b"\x01\x02", page=4) == b"\x01\x02\xFF\xFF"


def test_pad_to_page_keeps_exact_multiple():
    data = b"\x01\x02\x03\x04"
    assert fwbuild.pad_to_page(data, page=4) == data


def test_unit_firmware_image_is_page_padded(tmp_path):
    (tmp_path / "data").mkdir()
    (tmp_path / "data" / "unit-firmware.hex").write_text(
        _hex_line(0, b"\x01\x02\x03") + "\n" + EOF_RECORD + "\n",
        encoding="utf-8")
    image = fwbuild.unit_firmware_image(tmp_path)
    assert len(image) == 128 and image[:4] == b"\x01\x02\x03\xFF"


def test_version_tag():
    assert fwbuild.version_tag("abc1234", False) == "abc1234"
    assert fwbuild.version_tag("abc1234", True) == "abc1234-dirty"


def test_bundled_unit_rev_reads_sidecar_and_falls_back(tmp_path):
    assert fwbuild.bundled_unit_rev(tmp_path, "fallback") == "fallback"
    (tmp_path / "data").mkdir()
    (tmp_path / "data" / "unit-firmware.rev").write_text("0fd341f\n",
                                                         encoding="utf-8")
    assert fwbuild.bundled_unit_rev(tmp_path, "fallback") == "0fd341f"
    (tmp_path / "data" / "unit-firmware.rev").write_text("\n", encoding="utf-8")
    assert fwbuild.bundled_unit_rev(tmp_path, "fallback") == "fallback"


def test_equivalent_revs_skip_blank_and_comment_lines(tmp_path):
    assert fwbuild.bundled_unit_equivalent_revs(tmp_path) == ""
    (tmp_path / "data").mkdir()
    (tmp_path / "data" / "unit-firmware.equiv").write_text(
        "# proven\n34c72e0\n\n795f0af\n", encoding="utf-8")
    assert fwbuild.bundled_unit_equivalent_revs(tmp_path) == "34c72e0,795f0af"


def test_version_header_without_and_with_a_unit_bundle():
    plain = fwbuild.version_header_text("abc1234")
    assert '#define GIT_REV "abc1234"' in plain
    assert "BUNDLED_UNIT_REV" not in plain
    bundled = fwbuild.version_header_text("abc1234", "0fd341f", "a,b")
    assert '#define BUNDLED_UNIT_REV "0fd341f"' in bundled
    assert '#define BUNDLED_UNIT_REV_EQUIV "a,b"' in bundled


def test_compress_asset_is_reproducible():
    data = b"<html>hello</html>" * 20
    assert fwbuild.compress_asset(data) == fwbuild.compress_asset(data)
    assert gzip.decompress(fwbuild.compress_asset(data)) == data


def test_emit_array_shape():
    fh = io.StringIO()
    fwbuild.emit_array(fh, "BLOB", bytes(range(18)))
    text = fh.getvalue()
    assert text.startswith("const uint8_t BLOB[] PROGMEM = {")
    assert "const size_t BLOB_LEN = 18;" in text
    assert "0x11," in text
