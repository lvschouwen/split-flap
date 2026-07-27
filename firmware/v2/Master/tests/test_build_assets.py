"""Host-side tests for v2 Master/build_assets.py (#186, #205, #399).

Covered here: the console constants generated from the firmware headers
(#399 — the alphabet among them, which the UI used to hand-copy behind a
#149 drift gate), deterministic gzip (#168), the UTF-8 pinning guard, and —
since the reflash slice (#205) — the unit-firmware bundling (Intel-HEX parse,
page pad, rev sidecar) the v2 script re-grew from v1.

Run with:
    pytest tests/
"""

from __future__ import annotations

import pathlib
import sys

import pytest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))

import build_assets  # noqa: E402


# --- reading constants out of the headers ---------------------------------

# Deliberately NOT the real alphabet: these tests prove the parser, and a
# copy of the real value here would be the very duplication #399 removed.
EXPECTED_ALPHABET = " ABC$&#0123:.!?-"


def test_parse_header_alphabet_extracts_literal():
    header = f'#define SFP_ALPHABET "{EXPECTED_ALPHABET}"\n'
    assert build_assets.parse_header_alphabet(header) == EXPECTED_ALPHABET


def test_parse_header_alphabet_raises_when_missing():
    with pytest.raises(ValueError):
        build_assets.parse_header_alphabet("#define SOMETHING_ELSE 1\n")


def test_real_tree_no_longer_ships_the_old_panel():
    # #399: the five-tab panel was deleted, not parked on a second route.
    data = pathlib.Path(build_assets.__file__).resolve().parent / "data"
    for gone in ("index.html", "script.js", "style.css"):
        assert not (data / gone).exists(), f"{gone} came back"
    names = {name for name, _, _ in build_assets.ASSETS}
    assert names == {"console.html", "constants.js", "console.css", "console.js",
                     "console-detail.js", "portal.html", "md5.js", "favicon.png"}


def test_parse_int_define_reads_a_plain_number():
    assert build_assets.parse_int_define("#define SFP_I2C_ADDRESS_BASE 1\n",
                                         "SFP_I2C_ADDRESS_BASE") == 1


def test_parse_int_define_reads_a_bit_flag():
    assert build_assets.parse_int_define("#define UNIT_FLAG_HOMED (1 << 5)\n",
                                         "UNIT_FLAG_HOMED") == 32


def test_parse_int_define_ignores_a_trailing_comment():
    assert build_assets.parse_int_define(
        "#define SFP_OFFSET_LIMIT_STEPS     2038  // one full revolution\n",
        "SFP_OFFSET_LIMIT_STEPS") == 2038


def test_parse_int_define_raises_when_the_header_renamed_it():
    with pytest.raises(ValueError, match="GONE"):
        build_assets.parse_int_define("#define STILL_HERE 1\n", "GONE")


def test_shared_protocol_header_points_into_v2_shared(tmp_path):
    project = tmp_path / "v2" / "Master"
    assert build_assets.shared_protocol_header(project) == (
        tmp_path / "v2" / "shared" / "SplitFlapProtocol.h")


# --- generated console constants (#399) ------------------------------------
#
# The console cannot #include a C header, so every firmware constant it needs
# is GENERATED from the header that owns it. There is no second copy to drift,
# which is why the old hand-copied alphabet + drift gate are both gone.


def test_console_constants_come_from_the_real_headers():
    project = pathlib.Path(build_assets.__file__).resolve().parent
    c = build_assets.console_constants(project)
    header = build_assets.shared_protocol_header(project).read_text(encoding="utf-8")
    assert c["alphabet"] == build_assets.parse_header_alphabet(header)
    assert c["flapAmount"] == len(c["alphabet"])
    assert c["stepsPerFlap"] == c["stepsPerRevolution"] / c["flapAmount"]
    assert c["maxUnits"] > 0
    assert c["vccFloorMv"] > 0


def test_console_constants_carry_every_unit_flag_the_console_reads():
    project = pathlib.Path(build_assets.__file__).resolve().parent
    flags = build_assets.console_constants(project)["flag"]
    assert set(flags) == {"moving", "homeFailed", "hallNever", "addrEeprom", "homed"}
    # Distinct single bits — a duplicated mask would silently mis-colour units.
    assert sorted(flags.values()) == sorted(set(flags.values()))
    for v in flags.values():
        assert v and (v & (v - 1)) == 0


def test_build_constants_writes_a_loadable_module():
    project = pathlib.Path(build_assets.__file__).resolve().parent
    out = build_assets.build_constants(project)
    text = out.read_text(encoding="utf-8")
    assert text.startswith("/* Auto-generated")
    assert "var SFP = {" in text
    assert "module.exports = SFP" in text


def test_the_console_holds_no_copy_of_the_alphabet():
    # The whole point of generating: grep for the literal and find nothing.
    data = pathlib.Path(build_assets.__file__).resolve().parent / "data"
    project = data.parent
    alphabet = build_assets.console_constants(project)["alphabet"]
    for name in ("console.js", "console-detail.js", "console.html"):
        assert alphabet not in (data / name).read_text(encoding="utf-8"), name


# --- unit-firmware bundling (#205) -----------------------------------------


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
