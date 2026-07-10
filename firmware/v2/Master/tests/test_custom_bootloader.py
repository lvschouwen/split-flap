"""Pins the committed custom bootloader artifact (#201).

The bootloader is immutable over OTA and pioarduino never builds it from
sdkconfig, so the repo carries a prebuilt binary from firmware/v2/Bootloader
that Master's use_custom_bootloader.py embeds at 0x0. These tests keep that
artifact honest: the factory-reset code (#193) must actually be compiled in
(its absence was invisible until a bench test — the whole point of #201),
the flash header must match what the N16R8 module boots, and the image must
fit below the partition table.
"""

from pathlib import Path

import pytest

BOOTLOADER_BIN = (
    Path(__file__).resolve().parents[2]
    / "Bootloader"
    / "dist"
    / "bootloader-splitflap-esp32s3.bin"
)

# Config source of the artifact — the load-bearing lines are asserted so a
# careless edit can't silently change flash-critical behavior.
SDKCONFIG_DEFAULTS = (
    Path(__file__).resolve().parents[2] / "Bootloader" / "sdkconfig.defaults"
)


@pytest.fixture(scope="module")
def bootloader() -> bytes:
    assert BOOTLOADER_BIN.is_file(), (
        f"{BOOTLOADER_BIN} missing — build firmware/v2/Bootloader and copy "
        "the artifact (see its platformio.ini)"
    )
    return BOOTLOADER_BIN.read_bytes()


def test_image_magic(bootloader):
    assert bootloader[0] == 0xE9


def test_flash_header_matches_module(bootloader):
    # Byte 2 = flash mode as the ROM loads it (0x02 DIO — deliberate even on
    # QIO boards; the app switches later). Byte 3 = size|freq nibbles
    # (0x4F = 16 MB | 80 MHz). Both must equal the stock prebuilt's values
    # or the module won't boot.
    assert bootloader[2] == 0x02
    assert bootloader[3] == 0x4F


def test_fits_below_partition_table(bootloader):
    # Bootloader region on the S3 is 0x0..0x8000 (partition table offset).
    assert len(bootloader) <= 0x8000


def test_factory_reset_compiled_in(bootloader):
    # Strings from the CONFIG_BOOTLOADER_FACTORY_RESET block in
    # bootloader_start.c — present only when the feature is compiled in.
    assert b"Detect a condition of the factory reset" in bootloader
    assert b"Not all partitions were erased" in bootloader


def test_sdkconfig_pins_load_bearing_lines():
    # The factory-reset semantics (otadata-only erase, committable pin) are
    # guarded by test_partition_table.py against the same file; here only
    # the lines without a guard elsewhere.
    text = SDKCONFIG_DEFAULTS.read_text()
    # Rollback must never fall out of the custom build — the stock prebuilt
    # has it and the whole A/B OTA safety story (#187/#190) depends on it.
    assert "CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y" in text
    # The button pulls the pin LOW.
    assert "CONFIG_BOOTLOADER_FACTORY_RESET_PIN_LOW=y" in text
    # Flash geometry must match the N16R8 module.
    assert "CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y" in text
    assert "CONFIG_ESPTOOLPY_FLASHMODE_QIO=y" in text
    assert "CONFIG_ESPTOOLPY_FLASHFREQ_80M=y" in text
