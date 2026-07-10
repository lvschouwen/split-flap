"""Layout invariants for partitions_splitflap_16MB.csv (#187, #193).

The partition table is immutable after the first USB flash, and several
things must stay coherent with it:
  - the offsets gen_esp32part auto-derives from the blank-offset CSV must
    match what the header comment (and any already-flashed board) says;
  - `board_upload.arduino.boot_app0` in platformio.ini must equal the
    derived otadata offset (the prebuilt-Arduino builder otherwise
    hardcodes 0xe000);
  - the bootloader factory-reset config (#193) only makes sense alongside
    a `factory` app slot, and must never erase nvs (WiFi credentials are
    what make the rescue path useful).

These tests replicate gen_esp32part's auto-placement (first partition at
0x9000; data aligned to 0x1000, app to 0x10000) instead of shelling out
to it — the real binary is checked once per build on the bench.

Run with:
    pytest tests/
"""

from __future__ import annotations

import pathlib
import re

PROJECT_DIR = pathlib.Path(__file__).resolve().parent.parent
CSV_PATH = PROJECT_DIR / "partitions_splitflap_16MB.csv"
INI_PATH = PROJECT_DIR / "platformio.ini"

FLASH_SIZE = 0x1000000  # 16 MB
FIRST_OFFSET = 0x9000  # first free byte after the partition table itself
APP_ALIGN = 0x10000
DATA_ALIGN = 0x1000


def parse_partitions(csv_text: str) -> dict[str, dict]:
    """Parse the CSV and auto-place blank offsets like gen_esp32part does."""
    partitions: dict[str, dict] = {}
    cursor = FIRST_OFFSET
    for line in csv_text.splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        name, ptype, subtype, offset, size = [f.strip() for f in line.split(",")]
        align = APP_ALIGN if ptype == "app" else DATA_ALIGN
        if offset:
            cursor = int(offset, 0)
        elif cursor % align:
            cursor += align - cursor % align
        size_bytes = int(size, 0)
        partitions[name] = {
            "type": ptype,
            "subtype": subtype,
            "offset": cursor,
            "size": size_bytes,
        }
        cursor += size_bytes
    return partitions


def load_partitions() -> dict[str, dict]:
    return parse_partitions(CSV_PATH.read_text())


# --- placement is frozen for already-flashed boards ------------------------

EXPECTED_OFFSETS = {
    "nvs": 0x9000,
    "otadata": 0x19000,
    "app0": 0x20000,
    "app1": 0x420000,
    "coredump": 0x820000,
}


def test_pre_existing_offsets_unchanged():
    parts = load_partitions()
    for name, offset in EXPECTED_OFFSETS.items():
        assert parts[name]["offset"] == offset, name


def test_table_fits_16mb_flash_exactly():
    parts = load_partitions()
    end = max(p["offset"] + p["size"] for p in parts.values())
    assert end <= FLASH_SIZE


# --- factory rescue slot (#193) --------------------------------------------


def test_factory_slot_reserved_after_coredump():
    parts = load_partitions()
    factory = parts["factory"]
    assert factory["type"] == "app"
    assert factory["subtype"] == "factory"
    assert factory["size"] == 0x200000
    coredump = parts["coredump"]
    assert factory["offset"] >= coredump["offset"] + coredump["size"]
    assert factory["offset"] % APP_ALIGN == 0


def test_storage_shrunk_to_make_room():
    parts = load_partitions()
    assert parts["storage"]["size"] == 0x5D0000


# --- platformio.ini coherence -----------------------------------------------


def read_ini() -> str:
    return INI_PATH.read_text()


def test_boot_app0_matches_otadata_offset():
    match = re.search(r"board_upload\.arduino\.boot_app0\s*=\s*(0x[0-9A-Fa-f]+)", read_ini())
    assert match, "boot_app0 override missing from platformio.ini"
    assert int(match.group(1), 0) == load_partitions()["otadata"]["offset"]


# --- Rescue project coherence (#195) -----------------------------------------
# The rescue app shares nothing compiled with Master, but it boots from the
# same flash: its platformio.ini duplicates the boot_app0 override and points
# at Master's partition CSV. Pin both copies so a Master-side layout change
# can't silently drift away from a bench-proven rescue image.

RESCUE_INI_PATH = PROJECT_DIR.parent / "Rescue" / "platformio.ini"


def read_rescue_ini() -> str:
    return RESCUE_INI_PATH.read_text()


def test_rescue_boot_app0_matches_otadata_offset():
    match = re.search(
        r"board_upload\.arduino\.boot_app0\s*=\s*(0x[0-9A-Fa-f]+)", read_rescue_ini()
    )
    assert match, "boot_app0 override missing from Rescue's platformio.ini"
    assert int(match.group(1), 0) == load_partitions()["otadata"]["offset"]


def test_rescue_uses_masters_partition_csv():
    match = re.search(r"board_build\.partitions\s*=\s*(\S+)", read_rescue_ini())
    assert match, "partition table missing from Rescue's platformio.ini"
    assert (RESCUE_INI_PATH.parent / match.group(1)).resolve() == CSV_PATH.resolve()


def extract_sdkconfig(ini_text: str) -> str:
    """Return the custom_sdkconfig block (option lines until the next key)."""
    match = re.search(
        r"^custom_sdkconfig\s*=\s*(.*?)(?=^\S|\Z)", ini_text, re.MULTILINE | re.DOTALL
    )
    assert match, "custom_sdkconfig missing from platformio.ini"
    lines = [ln.strip() for ln in match.group(1).splitlines()]
    return "\n".join(ln for ln in lines if ln.startswith("CONFIG_"))


def test_bootloader_factory_reset_enabled():
    sdkconfig = extract_sdkconfig(read_ini())
    assert "CONFIG_BOOTLOADER_FACTORY_RESET=y" in sdkconfig
    assert "CONFIG_BOOTLOADER_OTA_DATA_ERASE=y" in sdkconfig


def test_factory_reset_pin_is_committable():
    sdkconfig = extract_sdkconfig(read_ini())
    match = re.search(r"CONFIG_BOOTLOADER_NUM_PIN_FACTORY_RESET=(\d+)", sdkconfig)
    assert match, "factory-reset pin not pinned in custom_sdkconfig"
    pin = int(match.group(1))
    # Not the ROM download-mode strap, not the octal-PSRAM pins, not the
    # native-USB data pins the console lives on.
    assert pin not in (0, 19, 20, 35, 36, 37)


def test_factory_reset_never_erases_nvs():
    # IDF defaults the data-erase list to "nvs" when factory reset is on, so
    # leaving it unset silently wipes WiFi credentials — the override to an
    # empty list must be explicit.
    sdkconfig = extract_sdkconfig(read_ini())
    match = re.search(r'CONFIG_BOOTLOADER_DATA_FACTORY_RESET="?([^"\n]*)"?', sdkconfig)
    assert match, "data-erase list must be explicitly emptied (IDF default is nvs)"
    assert "nvs" not in match.group(1), "rescue must keep WiFi credentials"
