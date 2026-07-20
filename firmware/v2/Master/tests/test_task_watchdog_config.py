"""TWDT config guard (#314).

The effective ESP_TASK_WDT_TIMEOUT_S lives in the generated (gitignored)
sdkconfig; the tracked source is platformio.ini's custom_sdkconfig. This
test pins that source so a future edit can't silently drop the 30 s override
or the panic-reboot behaviour the unattended wall depends on.

Run with:
    pytest tests/
"""

from __future__ import annotations

import pathlib
import re

PROJECT_DIR = pathlib.Path(__file__).resolve().parent.parent
INI_PATH = PROJECT_DIR / "platformio.ini"


def custom_sdkconfig_lines() -> list[str]:
    """Return the entries of the [env:master] custom_sdkconfig block."""
    text = INI_PATH.read_text()
    m = re.search(r"^custom_sdkconfig\s*=\s*$(.*?)^\S", text,
                  re.MULTILINE | re.DOTALL)
    assert m, "custom_sdkconfig block not found in platformio.ini"
    lines = []
    for raw in m.group(1).splitlines():
        stripped = raw.strip()
        if stripped and not stripped.startswith(";"):
            lines.append(stripped)
    return lines


def test_task_wdt_timeout_is_30s():
    assert "CONFIG_ESP_TASK_WDT_TIMEOUT_S=30" in custom_sdkconfig_lines()


def test_task_wdt_panic_not_disabled():
    # PANIC defaults on (framework) and must not be turned off here — a
    # timeout must reboot, not just warn.
    assert "CONFIG_ESP_TASK_WDT_PANIC=n" not in custom_sdkconfig_lines()
