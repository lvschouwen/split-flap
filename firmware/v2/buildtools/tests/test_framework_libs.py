"""framework_libs: the shared IDF lib build checked against a custom_sdkconfig (#634)."""

from __future__ import annotations

import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))

from framework_libs import custom_entries, missing  # noqa: E402

MASTER = """
  CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y
  ; a comment the ini keeps
  CONFIG_ESP_TASK_WDT_TIMEOUT_S=30
  CONFIG_ESP_PANIC_HANDLER_IRAM=y
  CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y
"""

# What the libs said on 2026-10-19 after Rescue had compiled them.
RESCUE_LIBS = """
CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y
CONFIG_ESP_TASK_WDT_TIMEOUT_S=5
# CONFIG_ESP_PANIC_HANDLER_IRAM is not set
CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y
CONFIG_TASK_WDT_TIMEOUT_S=5
"""

MASTER_LIBS = RESCUE_LIBS.replace("TIMEOUT_S=5", "TIMEOUT_S=30").replace(
    "# CONFIG_ESP_PANIC_HANDLER_IRAM is not set", "CONFIG_ESP_PANIC_HANDLER_IRAM=y")


def test_libs_compiled_by_a_sibling_are_caught():
    assert missing(MASTER, RESCUE_LIBS) == [
        "CONFIG_ESP_TASK_WDT_TIMEOUT_S=30 (libs: 5)",
        "CONFIG_ESP_PANIC_HANDLER_IRAM=y (libs: n)",
    ]


def test_libs_compiled_for_the_project_pass():
    assert missing(MASTER, MASTER_LIBS) == []


def test_an_option_absent_from_the_libs_counts_as_off():
    assert missing("CONFIG_NEW_THING=y", "CONFIG_OTHER=y") == ["CONFIG_NEW_THING=y (libs: n)"]
    assert missing("# CONFIG_NEW_THING is not set", "CONFIG_OTHER=y") == []


def test_quoted_and_string_values():
    assert custom_entries("'CONFIG_A=\"x\"'\nCONFIG_B=7") == {"CONFIG_A": '"x"', "CONFIG_B": "7"}


def test_comments_are_not_options():
    assert custom_entries("; CONFIG_A=y\n# just a note") == {}
