"""patch_asyncweb: a response ends only once its last piece is queued (#631).

The fixtures are the pinned v3.12.1 source the patch is anchored to.
"""

from __future__ import annotations

import pathlib
import re
import shutil
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))

import patch_asyncweb  # noqa: E402

FIXTURES = pathlib.Path(__file__).resolve().parent / "fixtures" / "asyncweb-3.12.1"
RESPONSE_FILES = {patch_asyncweb._RESP, patch_asyncweb._RESP_H}
RESPONSE_PATCHES = [p for p in patch_asyncweb.PATCHES if p[0] in RESPONSE_FILES]


def _patched(tmp_path: pathlib.Path) -> str:
    src = tmp_path / "src"
    shutil.copytree(FIXTURES, src)
    patch_asyncweb.apply_patches(src, RESPONSE_PATCHES)
    return (src / patch_asyncweb._RESP).read_text()


def _content_loop(text: str) -> str:
    start = text.index("if (_state == RESPONSE_CONTENT) {")
    return text[start:text.index("// implicit check", start)]


def _ends_inside_loop(text: str) -> list[str]:
    """What sets RESPONSE_END while the body is sent, with the line before it."""
    lines = _content_loop(text).splitlines()
    return [lines[i - 1].strip() for i, line in enumerate(lines)
            if re.search(r"_state\s*=\s*RESPONSE_END", line)]


def test_the_pinned_source_ends_a_response_before_its_last_piece_is_queued():
    # The bug the patch is for: the source's end sets RESPONSE_END at once.
    pinned = (FIXTURES / patch_asyncweb._RESP).read_text()
    assert len(_ends_inside_loop(pinned)) == 3


def test_a_response_ends_only_with_nothing_left_in_its_buffer(tmp_path):
    text = _patched(tmp_path)
    assert _ends_inside_loop(text) == ["if (_sf631SourceEnded && !_send_buffer_len) {"]
    assert "if (tcp_win == 0 || _sf631SourceEnded) {" in text
    assert text.count("_sf631SourceEnded = true;") == 3
    header = (tmp_path / "src" / patch_asyncweb._RESP_H).read_text()
    assert "bool _sf631SourceEnded{false};" in header


def test_a_credit_is_taken_only_for_bytes_put_in_flight(tmp_path):
    text = _patched(tmp_path)
    tail = _content_loop(text).split("_writtenLength += payloadlen;", 1)[1]
    assert re.search(r"if \(payloadlen\) \{[^}]*--_in_flight_credit;", tail)


def test_a_second_run_changes_nothing(tmp_path):
    once = _patched(tmp_path)
    patch_asyncweb.apply_patches(tmp_path / "src", RESPONSE_PATCHES)
    assert (tmp_path / "src" / patch_asyncweb._RESP).read_text() == once
