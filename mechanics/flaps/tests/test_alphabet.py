import json

import pytest

from flaps.alphabet import COLOUR_SLOTS, Entry, load_alphabet


def test_alphabet_is_the_note_set():
    a = load_alphabet()
    assert len(a) == 64 and len(set(a)) == 64
    assert a[0] == Entry("blank", "")
    assert a[1].value == "A" and a[26].value == "Z"
    assert "".join(e.value for e in a[27:37]) == "1234567890"
    assert "".join(e.value for e in a[37:56]) == "!@#$()-+&=;:'\"%,./?"
    assert a[56] == Entry("heart", "")
    assert [e.value for e in a[57:]] == ["red", "orange", "yellow", "green", "blue", "violet", "white"]
    assert all(e.kind == "colour" for e in a[57:])


def test_colour_slots():
    assert COLOUR_SLOTS == {"black": 1, "white": 2, "red": 3, "orange": 4, "yellow": 5,
                            "green": 6, "blue": 7, "violet": 8}


def _good():
    return [{"kind": e.kind, "value": e.value} for e in load_alphabet()]


def _short(rows):
    return rows[:63]


def _dup(rows):
    rows[2] = rows[1]
    return rows


def _bad_kind(rows):
    rows[1] = {"kind": "glyph", "value": "A"}
    return rows


def _bad_colour(rows):
    rows[57] = {"kind": "colour", "value": "pink"}
    return rows


@pytest.mark.parametrize("mutate", [_short, _dup, _bad_kind, _bad_colour])
def test_alphabet_rejects_bad_files(tmp_path, mutate):
    f = tmp_path / "a.json"
    f.write_text(json.dumps(mutate(_good())))
    with pytest.raises(ValueError):
        load_alphabet(f)
