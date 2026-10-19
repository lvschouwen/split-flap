"""Does the shared Arduino IDF lib build carry this project's custom_sdkconfig?

pioarduino compiles the Arduino IDF libs once into the shared package
directory (framework-arduinoespressif32-libs/<mcu>), and decides whether to
compile them again from a hash in the project's own sdkconfig.defaults. It
never reads the shared libs back, so a sibling project with another
custom_sdkconfig can replace them while this project's hash still matches: the
build then links libs without this project's settings (#634: a 5 s task
watchdog instead of 30 s). This module compares the keys; check_framework_libs.py
acts on the result during the build.
"""

from __future__ import annotations

import re

_SET = re.compile(r"^(CONFIG_[A-Za-z0-9_]+)=(.*)$")
_NOT_SET = re.compile(r"^# (CONFIG_[A-Za-z0-9_]+) is not set$")


def custom_entries(custom_sdkconfig: str) -> dict[str, str]:
    """KEY -> value of a custom_sdkconfig option; `n` for an option turned off."""
    entries: dict[str, str] = {}
    for raw in custom_sdkconfig.splitlines():
        line = raw.strip().replace("'", "")
        set_match = _SET.match(line)
        if set_match:
            entries[set_match.group(1)] = set_match.group(2).strip()
            continue
        not_set = _NOT_SET.match(line)
        if not_set:
            entries[not_set.group(1)] = "n"
    return entries


def lib_values(sdkconfig: str) -> dict[str, str]:
    """KEY -> value of the sdkconfig the libs were compiled with."""
    return custom_entries(sdkconfig)


def missing(custom_sdkconfig: str, lib_sdkconfig: str) -> list[str]:
    """The custom options the libs were not compiled with, as KEY=wanted (has)."""
    have = lib_values(lib_sdkconfig)
    out = []
    for key, wanted in custom_entries(custom_sdkconfig).items():
        has = have.get(key, "n")
        if has != wanted:
            out.append(f"{key}={wanted} (libs: {has})")
    return out
