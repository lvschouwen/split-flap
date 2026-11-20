from flaps.alphabet import Entry, load_alphabet
from flaps.layout import display_to_flap, faces_of_flap
from flaps.params import FlapParams


def test_pairing_wraps_and_colour_flaps():
    a = load_alphabet()
    assert faces_of_flap(0, a) == (a[0], a[1])
    assert faces_of_flap(63, a) == (a[63], a[0])
    assert faces_of_flap(56, a) == (Entry("heart", ""), Entry("colour", "red"))


def test_front_half_mirrors_x_and_offsets_by_pivot():
    # The front prints on the bed (z=0) and is read from -z: screen right is -x.
    face, x, y = display_to_flap(3.0, 10.0, FlapParams())
    assert (face, x) == ("front", -3.0) and abs(y - 11.8) < 1e-9


def test_back_half_mirrors_x_too():
    # Fallen flap = 180 deg about the hinge (x) axis; its back is read from -z as well.
    face, x, y = display_to_flap(-4.0, -10.0, FlapParams())
    assert (face, x) == ("back", 4.0) and abs(y - 11.8) < 1e-9


def test_midline_is_front():
    assert display_to_flap(0.0, 0.0, FlapParams())[0] == "front"
