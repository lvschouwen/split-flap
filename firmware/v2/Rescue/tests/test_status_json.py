"""Source gate: /rescue/status names an image by its rev only (#558).

The app descriptor's version and build date are compiled into the framework
libraries, so every image carries the same ones; shown per slot they read as
if a clean build were a dirty one.
"""
import re
from pathlib import Path

SRC = Path(__file__).resolve().parent.parent / "RescueWeb.cpp"


def _code():
    return re.sub(r"//[^\n]*", "", SRC.read_text())


def test_no_descriptor_stamp_leaves_the_board():
    # The build date still ranks the exit slot when no record does; it is
    # never written into an answer.
    code = _code()
    start = code.index("static void appendSlotJson(")
    end = code.index("return out;", code.index("static String statusJson("))
    assert "desc." not in code[start:end]


def test_the_running_slot_is_named_by_this_images_rev():
    code = _code()
    slot = code[code.index("static void appendSlotJson("):code.index("static String statusJson(")]
    assert '} else if (running) {' in slot
    assert 'GIT_REV' in slot
