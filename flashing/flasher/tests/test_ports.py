from unittest.mock import patch
from flasher.ports import DRIVER_HINT, diff_new, wait_for_new_port


def test_diff_new():
    assert diff_new({"COM3"}, {"COM3", "COM5"}) == {"COM5"}
    assert diff_new({"COM3"}, {"COM3"}) == set()
    assert diff_new({"COM3"}, set()) == set()


def test_wait_for_new_port_returns_first_appearance():
    seq = [["COM3"], ["COM3"], ["COM3", "COM7"]]
    with patch("flasher.ports.list_port_names", side_effect=seq):
        with patch("flasher.ports.time.sleep"):
            assert wait_for_new_port({"COM3"}, timeout=10, poll=0) == "COM7"


def test_wait_for_new_port_times_out():
    with patch("flasher.ports.list_port_names", return_value=["COM3"]):
        with patch("flasher.ports.time.sleep"):
            assert wait_for_new_port({"COM3"}, timeout=0.01, poll=0) is None


def test_driver_hint_mentions_ch340():
    assert "CH340" in DRIVER_HINT
