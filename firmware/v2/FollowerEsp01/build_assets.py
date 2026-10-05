"""PlatformIO pre-build script for the ESP-01 row board (#298).

No web assets — this firmware serves no HTML. Generates:
  - UnitAssets.h: PROGMEM array of the bundled unit firmware binary
    (data/unit-firmware.hex, staged by flasher/make_manifest.py stage —
    MUST run between the Unit build and this build).
  - BuildVersion.h: GIT_REV + BUNDLED_UNIT_REV.

Post-build it stamps the artifact as follower-<rev>.bin — the prefix is
what ota-flash.sh keys the ESP-01 platform on (never a valid payload for an
S3 master's /firmware/master and vice versa) — and as follower-<rev>-gz.bin,
the same image gzip-packed for OTA (#540), and prints how far the image is
from the size at which a row could no longer update itself.
"""

import pathlib
import sys

# PlatformIO runs this file as a pre-build script: it sets up a SCons env and
# provides Import() as a builtin. A plain `import build_assets` (pytest) has
# neither, and then only defines the functions below.
try:
    Import("env")  # noqa: F821  (provided by PlatformIO SCons env)
    _UNDER_SCONS = True
    _PROJECT_DIR = pathlib.Path(env["PROJECT_DIR"])  # noqa: F821
except NameError:
    _UNDER_SCONS = False
    _PROJECT_DIR = pathlib.Path(__file__).resolve().parent

# The helpers every tree shares (hex parsing, rev stamping, PROGMEM arrays).
sys.path.insert(0, str(_PROJECT_DIR.parent / "buildtools"))
from fwbuild import (  # noqa: E402
    GENERATED_BANNER,
    emit_array,
    stamp_follower_images,
    unit_firmware_image,
    write_version_header,
)


def build_unit_assets(project_dir: pathlib.Path) -> None:
    unit_hex = project_dir / "data" / "unit-firmware.hex"
    unit_bin = unit_firmware_image(project_dir)
    output = project_dir / "UnitAssets.h"
    with output.open("w", encoding="utf-8") as fh:
        fh.write(GENERATED_BANNER)
        fh.write("#pragma once\n\n#include <Arduino.h>\n\n")
        emit_array(fh, "UNIT_FIRMWARE_BIN", unit_bin)
    print(f"[build_assets] wrote {output.name}  "
          f"unit-firmware hex {unit_hex.stat().st_size} -> bin {len(unit_bin)}")


if _UNDER_SCONS:
    _tag = write_version_header(_PROJECT_DIR, with_unit_bundle=True)
    build_unit_assets(_PROJECT_DIR)
    stamp_follower_images(env, _tag)  # noqa: F821
