import pytest
from flasher.wiring import DIAGRAMS, dip_pattern, dip_visual


@pytest.mark.parametrize("unit,expected", [(1, "0000"), (2, "0001"), (3, "0010"), (10, "1001"), (16, "1111")])
def test_dip_pattern_matches_readme_table(unit, expected):
    assert dip_pattern(unit) == expected


@pytest.mark.parametrize("bad", [0, 17, -1])
def test_dip_pattern_rejects_out_of_range(bad):
    with pytest.raises(ValueError):
        dip_pattern(bad)


def test_dip_visual_marks_up_switches():
    v = dip_visual(3)  # 0010 -> SW3 up
    assert "0010" in v and "up" in v


def test_all_diagrams_present_and_substantial():
    for key in ("programmer", "icsp", "esp_uart", "assembly"):
        assert key in DIAGRAMS and len(DIAGRAMS[key]) > 100


def test_key_wiring_facts():
    assert "D10" in DIAGRAMS["icsp"]
    assert "10uF" in DIAGRAMS["programmer"] or "10 µF" in DIAGRAMS["programmer"]
    assert "3.3V" in DIAGRAMS["esp_uart"]
    assert "GPIO0" in DIAGRAMS["esp_uart"]
    assert "SDA" in DIAGRAMS["assembly"]
