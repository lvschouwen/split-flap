"""Source gate for the CPU clock (#683).

The row runs at 160 MHz: a faster loop() and software I2C to the units. The
core derives its I2C half-cycle (the 100 kHz bus the twiboot reflash needs)
and the line test reads the clock at run time, so nothing else is hard-wired;
this pins the one setting.
"""
import re
from pathlib import Path

TREE = Path(__file__).resolve().parents[1]


def test_the_follower_runs_at_160_mhz():
    ini = (TREE / "platformio.ini").read_text()
    follower = ini.split("[env:follower]")[1].split("[env:")[0]
    settings = re.findall(r"^board_build\.f_cpu\s*=\s*(\S+)\s*$", follower, re.M)
    assert settings == ["160000000L"]


def test_no_source_hardwires_the_clock():
    for p in TREE.glob("*"):
        if p.suffix in (".cpp", ".h") and p.name != "UnitAssets.h":
            text = p.read_text()
            assert "F_CPU" not in text and "80000000" not in text, p.name
