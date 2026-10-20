"""Source gate for IPv6 on the row board (#637, part of #612).

The core's IPv6 stack is chosen at build time, so the build flag is the whole
switch: the row then takes link-local and SLAAC addresses by itself.
"""
from pathlib import Path

TREE = Path(__file__).resolve().parents[1]


def test_the_image_is_built_with_the_ipv6_stack():
    ini = (TREE / "platformio.ini").read_text()
    follower = ini.split("[env:follower]")[1].split("[env:")[0]
    flags = [line.strip() for line in follower.splitlines()]
    assert "-DPIO_FRAMEWORK_ARDUINO_LWIP2_IPV6_LOW_MEMORY" in flags
