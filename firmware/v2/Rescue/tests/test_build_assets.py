"""Tests for build_assets.py (#195) — the rescue project's pre-build bake:
BuildVersion.h (git rev) + RescueAssets.h (gzipped PROGMEM page + md5.js).
Trimmed COPY of Master's build_assets.py (no alphabet check, no favicon).
Run from firmware/v2/Rescue: python -m pytest tests/
"""

import gzip
import io
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))

from build_assets import (
    ASSETS,
    build_version_header,
    compress_asset,
    emit_array,
    version_tag,
)


def test_clean_rev_is_bare_hash():
    assert version_tag("abc1234", False) == "abc1234"


def test_dirty_rev_gets_suffix():
    assert version_tag("abc1234", True) == "abc1234-dirty"


def test_header_defines_git_rev():
    header = build_version_header("abc1234-dirty")
    assert '#define GIT_REV "abc1234-dirty"' in header
    assert "#pragma once" in header
    assert "do not edit" in header.lower()


def test_compress_is_deterministic():
    # mtime=0: two bakes of the same source must be byte-identical so the
    # firmware bin is reproducible per commit (Master #168 rationale).
    data = b"<html>rescue</html>" * 100
    assert compress_asset(data) == compress_asset(data)


def test_compress_roundtrips():
    data = b"var SparkMD5 = {};"
    assert gzip.decompress(compress_asset(data)) == data


def test_emit_array_shape():
    fh = io.StringIO()
    emit_array(fh, "RESCUE_HTML_GZ", b"\x1f\x8b\x00")
    out = fh.getvalue()
    assert "const uint8_t RESCUE_HTML_GZ[] PROGMEM" in out
    assert "0x1F, 0x8B, 0x00," in out
    assert "const size_t RESCUE_HTML_GZ_LEN = 3;" in out


def test_assets_list_covers_page_and_md5():
    files = [a[0] for a in ASSETS]
    assert "rescue.html" in files
    assert "md5.js" in files
