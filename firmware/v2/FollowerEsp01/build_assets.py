"""PlatformIO pre-build script for the ESP-01 follower (#298).

No web assets — this firmware serves no HTML. Generates:
  - UnitAssets.h: PROGMEM array of the bundled unit firmware binary
    (data/unit-firmware.hex, staged by flasher/make_manifest.py stage —
    MUST run between the Unit build and this build).
  - BuildVersion.h: GIT_REV + BUNDLED_UNIT_REV.
  - ApiIndexAsset.h: the whole GET /api reply as a PROGMEM array, built from
    ApiIndex.h's tables (#519). On the ESP-01 those tables' string literals
    and the buffer the reply was assembled in cost ~10 KB of an 82 KB RAM
    budget; served from flash they cost none.

Post-build it stamps the artifact as follower-<rev>.bin — the prefix is
what ota-flash.sh keys the ESP-01 platform on (never a valid payload for an
S3 master's /firmware/master and vice versa) — and as follower-<rev>-gz.bin,
the same image gzip-packed for OTA (#540), and prints how far the image is
from the size at which a row could no longer update itself.
"""

import pathlib
import re
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


_C_STRING = r'"((?:[^"\\]|\\.)*)"'


def _table_block(src: str, name: str) -> str:
    """The initialiser of `name[] = { ... };`, with // comments removed."""
    m = re.search(re.escape(name) + r"\[\]\s*=\s*\{(.*?)\n\};", src, re.DOTALL)
    if not m:
        raise ValueError(f"{name} table not found in ApiIndex.h")
    return re.sub(r"^\s*//.*$", "", m.group(1), flags=re.MULTILINE)


def api_index_json(project_dir: pathlib.Path) -> str:
    """The exact text buildApiJson() in ApiIndex.h produces, from the same
    tables. tests/test_api_index_asset.py compiles that function on the host
    and compares the two byte for byte, so this cannot drift from it."""
    src = (project_dir / "ApiIndex.h").read_text(encoding="utf-8")
    routes = re.findall(r"\{\s*%s\s*,\s*%s\s*,\s*%s\s*\}" % ((_C_STRING,) * 3),
                        _table_block(src, "API_ROUTES"))
    not_served = re.findall(_C_STRING, _table_block(src, "API_NOT_SERVED"))
    legend = re.findall(r"\{\s*%s\s*,\s*%s\s*\}" % ((_C_STRING,) * 2),
                        _table_block(src, "API_LEGEND"))
    if not routes or not legend:
        raise ValueError("ApiIndex.h tables parsed empty")
    for text in [f for row in routes + legend for f in row] + not_served:
        if "\\" in text or '"' in text:
            # buildApiJson() prints the literals verbatim; an escape would need
            # the same treatment on both sides.
            raise ValueError(f"escape sequence in an ApiIndex.h string: {text!r}")
    out = '{"routes":['
    out += ",".join('{"m":"%s","p":"%s","d":"%s"}' % r for r in routes)
    out += '],"notServed":['
    out += ",".join('"%s"' % p for p in not_served)
    out += '],"legend":{'
    out += ",".join('"%s":"%s"' % kv for kv in legend)
    out += "}}"
    return out


def build_api_index_asset(project_dir: pathlib.Path) -> None:
    body = api_index_json(project_dir).encode("utf-8")
    output = project_dir / "ApiIndexAsset.h"
    with output.open("w", encoding="utf-8") as fh:
        fh.write(GENERATED_BANNER)
        fh.write("#pragma once\n\n#include <Arduino.h>\n\n")
        emit_array(fh, "API_INDEX_JSON", body)
    print(f"[build_assets] wrote {output.name}  {len(body)} bytes")


if _UNDER_SCONS:
    _tag = write_version_header(_PROJECT_DIR, with_unit_bundle=True)
    build_unit_assets(_PROJECT_DIR)
    build_api_index_asset(_PROJECT_DIR)
    stamp_follower_images(env, _tag)  # noqa: F821
