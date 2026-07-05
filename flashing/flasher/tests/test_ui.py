from unittest.mock import patch
from flasher.ui import ask_int, ask_yn


def test_ask_int_reprompts_until_in_range():
    with patch("builtins.input", side_effect=["0", "banana", "17", "12"]):
        assert ask_int("units?", 1, 16) == 12


def test_ask_yn_defaults():
    with patch("builtins.input", return_value=""):
        assert ask_yn("sure?", default=True) is True
        assert ask_yn("sure?", default=False) is False
    with patch("builtins.input", return_value="y"):
        assert ask_yn("sure?", default=False) is True
