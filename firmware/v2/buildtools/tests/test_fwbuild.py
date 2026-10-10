"""Tests for the shared build helpers (#535).
Run from firmware/v2/buildtools: python -m pytest tests/
"""

import gzip
import io
import pathlib
import subprocess
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
    assert "#define GIT_COMMIT_TIME 0UL" in plain
    assert "#define GIT_COMMIT_TIME 1791622102UL" in fwbuild.version_header_text(
        "abc1234", commit_time=1791622102)
    bundled = fwbuild.version_header_text("abc1234", "0fd341f", "a,b")
    assert '#define BUNDLED_UNIT_REV "0fd341f"' in bundled
    assert '#define BUNDLED_UNIT_REV_EQUIV "a,b"' in bundled


def test_commit_time_is_the_head_commits_and_zero_without_git(tmp_path):
    here = pathlib.Path(__file__).resolve().parent
    head = int(subprocess.check_output(
        ["git", "log", "-1", "--format=%ct"], cwd=here).decode())
    assert fwbuild.git_commit_time(here) == head > 1_700_000_000
    assert fwbuild.git_commit_time(tmp_path) == 0


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


# --- ESP-01 OTA images (#540) ----------------------------------------------------

def _image(n: int) -> bytes:
    """An ESP8266-shaped image of `n` bytes that compresses like code does
    (not at all like zeros, not at all like noise)."""
    import random
    rng = random.Random(540)
    words = [bytes(rng.randrange(256) for _ in range(4)) for _ in range(900)]
    body = b"".join(rng.choice(words) for _ in range(n // 4 + 1))
    return (b"\xE9\x02\x00\x20" + body)[:n]


def test_ota_gzip_is_a_gzip_file_of_the_image():
    raw = _image(70000)
    assert gzip.decompress(fwbuild.ota_gzip_image(raw)) == raw


def test_ota_gzip_leaves_the_image_header_readable():
    # The exact prefix FollowerOtaImage.h parses (and its native test builds
    # by hand): gzip header without optional fields, one stored block.
    raw = _image(70000)
    packed = fwbuild.ota_gzip_image(raw)
    assert packed[:15] == bytes.fromhex("1f8b0800" "00000000" "02ff"
                                        "00" "1000" "efff")
    off = fwbuild.OTA_GZIP_IMAGE_OFFSET
    assert packed[off:off + fwbuild.OTA_GZIP_STORED_LEN] == raw[:16]


def test_ota_gzip_is_reproducible():
    raw = _image(70000)
    assert fwbuild.ota_gzip_image(raw) == fwbuild.ota_gzip_image(raw)


def test_ota_gzip_tail_is_the_unpacked_length():
    raw = _image(70001)
    assert int.from_bytes(fwbuild.ota_gzip_image(raw)[-4:], "little") == 70001


def test_ota_max_upload_is_free_space_minus_a_sector():
    # 453776 B running -> 111 sectors used, 140 free, the handler keeps one.
    assert fwbuild.ota_max_upload(453776) == 139 * 4096
    assert fwbuild.ota_max_upload(fwbuild.ESP01_SKETCH_AREA) == 0
    assert fwbuild.ota_max_upload(fwbuild.ESP01_SKETCH_AREA - 1) == 0


def test_plain_ceiling_is_half_the_area_less_the_spare_sector():
    ceiling = fwbuild.ota_ceiling(1.0)
    assert ceiling == 125 * 4096 - fwbuild.OTA_MULTIPART_ALLOWANCE
    assert fwbuild.ota_upload_fits(ceiling, ceiling)
    assert not fwbuild.ota_upload_fits(ceiling + 1, ceiling + 1)


def test_gzip_ceiling_is_above_the_plain_one_and_exact():
    ceiling = fwbuild.ota_ceiling(0.7)
    assert ceiling > fwbuild.ota_ceiling(1.0) + 80000
    assert fwbuild.ota_can_replace(ceiling, ceiling, round(ceiling * 0.7))
    over = ceiling + 4096
    assert not fwbuild.ota_can_replace(over, over, round(over * 0.7))


def test_gzip_upload_is_stored_at_the_top_of_the_app_area():
    # 317842 B + framing -> 78 sectors, ending where the app area ends.
    assert fwbuild.ota_stage_addr(453776, 317842, True) == 0xFB000 - 78 * 4096
    # A plain image takes the whole free space: one sector past the image.
    assert fwbuild.ota_stage_addr(453776, 453776, False) == 112 * 4096


def test_gzip_image_must_unpack_below_its_stored_copy():
    assert fwbuild.ota_gzip_unpack_fits(453776, 453776, 317842)
    assert fwbuild.ota_gzip_unpack_fits(453776, 708608, 317842)
    assert not fwbuild.ota_gzip_unpack_fits(453776, 708609, 317842)


def test_ota_report_names_both_margins():
    line = fwbuild.ota_report(453776, 317842)
    assert "plain-OTA ceiling 510976 B (+57200)" in line
    assert "gzip-OTA ceiling" in line and "70.0%" in line
