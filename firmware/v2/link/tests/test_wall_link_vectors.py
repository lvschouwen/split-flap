"""Holds link/wall_link.py to WallLink.h.

The native test (Master/test/test_wall_link) asserts the C++ encoders against
the VEC byte vectors written in it. This reads the same vectors out of that
file and requires the Python twin to produce them, so the two codecs cannot
drift apart; it also compares the type numbers and limits with the header.
"""
import re
import sys
from pathlib import Path

import pytest

LINK = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(LINK))
import wall_link  # noqa: E402

HEADER = (LINK / "WallLink.h").read_text()
NATIVE_TEST = (LINK.parent / "Master/test/test_wall_link/test_wall_link.cpp").read_text()
VEC = dict(re.findall(r'VEC\("(\w+)",\s*"([0-9a-f]*)"\)', NATIVE_TEST))


def test_every_vector_exists_on_both_sides():
    assert set(VEC) == set(wall_link.VECTORS)
    covered = {message for message, _ in wall_link.VECTORS.values()}
    assert covered == set(wall_link.MESSAGES), "a message has no vector"


@pytest.mark.parametrize("name", sorted(wall_link.VECTORS))
def test_python_encodes_the_bytes_the_native_test_asserts(name):
    assert wall_link.vector_bytes(name).hex() == VEC[name]


@pytest.mark.parametrize("name", sorted(wall_link.VECTORS))
def test_python_decodes_what_it_encodes(name):
    message, fields = wall_link.VECTORS[name]
    (number, payload), = wall_link.Decoder().feed(wall_link.vector_bytes(name))
    decoded_name, decoded = wall_link.decode(number, payload)
    assert decoded_name == message
    for key, value in fields.items():
        expected = value.encode() if key == "text" and message == "logline" else value
        assert decoded[key] == expected


def test_type_numbers_match_the_header():
    enum = re.search(r"enum class WlType : uint8_t \{(.*?)\};", HEADER, re.S).group(1)
    numbers = {name.lower(): int(value) for name, value in re.findall(r"(\w+) = (\d+),", enum)}
    assert numbers == {name: number for name, (number, _) in wall_link.MESSAGES.items()}


def test_limits_match_the_header():
    def constant(name):
        return int(re.search(rf"constexpr \w+ {name} = (\d+);", HEADER).group(1))

    assert wall_link.PROTOCOL == constant("WALL_LINK_PROTOCOL")
    assert wall_link.PORT == constant("WALL_LINK_PORT")
    assert wall_link.HEADER_LEN == constant("WL_HEADER_LEN")
    assert wall_link.MAX_PAYLOAD == constant("WL_MAX_PAYLOAD")
    limits = {"s32": "WL_ID_MAX", "s16": "WL_REV_MAX", "s64": "WL_TZ_MAX",
              "a16": "WL_OP_ARGS_MAX", "d480": "WL_OP_DATA_MAX"}
    for kind, name in limits.items():
        assert int(kind[1:]) == constant(name)
    assert constant("WL_TEXT_MAX") == 16 and constant("WL_REV_MAX") == 16


def test_decoder_reassembles_split_frames_and_rejects_an_oversized_one():
    stream = wall_link.vector_bytes("pong") + wall_link.vector_bytes("show")
    decoder, frames = wall_link.Decoder(), []
    for i in range(len(stream)):
        frames += decoder.feed(stream[i:i + 1])
    assert [wall_link.TYPE_NAMES[number] for number, _ in frames] == ["pong", "show"]
    with pytest.raises(wall_link.LinkError):
        wall_link.Decoder().feed(bytes([0x02, 0x01, 3]))


def test_a_field_over_its_limit_is_refused_not_cut():
    with pytest.raises(wall_link.LinkError):
        wall_link.encode("show", renderId=1, atMs=0, speed=1, text="A" * 17)
    payload = bytes([0, 0, 0, 1, 0, 0, 0, 0, 1, 17]) + b"A" * 17
    with pytest.raises(wall_link.LinkError):
        wall_link.decode(3, payload)


def test_decode_ignores_fields_added_at_the_end():
    _, fields = wall_link.decode(39, bytes([0, 7, 0xAB, 0xCD]))
    assert fields == {"rxCount": 7}
