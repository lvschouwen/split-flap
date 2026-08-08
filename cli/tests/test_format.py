from splitflap_tui.format import human_duration, human_size


def test_human_size_small_passthrough():
    assert human_size(512) == "512"
    assert human_size(0) == "0"


def test_human_size_kilo_and_mega():
    assert human_size(182432) == "182k"
    assert human_size(186432) == "186k"
    assert human_size(1_800_000) == "1.8M"


def test_human_duration_tiers():
    assert human_duration(45) == "45s"
    assert human_duration(125) == "2m 5s"
    assert human_duration(7500) == "2h 5m"
    assert human_duration(271234) == "3d 3h"
