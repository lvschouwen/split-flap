"""PlatformIO pre-build script for the v2 master (#186).

Generates:
  - WebAssets.h: PROGMEM arrays for the page (web/, joined by web_bundle.py),
    the WiFi portal page and the icon.
  - BuildVersion.h: #define for the current git commit (short hash +
    dirty flag), so the master firmware can report the version that
    was actually built into it.

Since the reflash slice (#205) it also bundles the unit firmware: data/
unit-firmware.hex (+ .rev sidecar, both committed — staged by
flashing/flasher/make_manifest.py) becomes UNIT_FIRMWARE_BIN in WebAssets.h
and BUNDLED_UNIT_REV in BuildVersion.h. The UI is served straight from
PROGMEM — no filesystem image, no uploadfs step.

Invoked by PlatformIO via `extra_scripts = pre:build_assets.py`.
"""

import csv
import json
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
sys.path.insert(0, str(_PROJECT_DIR))
import web_bundle  # noqa: E402
from fwbuild import (  # noqa: E402
    GENERATED_BANNER,
    bundled_unit_equivalent_revs,
    bundled_unit_rev,
    compress_asset,
    emit_array,
    pad_to_page,
    stamp_firmware_bin,
    unit_firmware_image,
    write_version_header,
)

ASSETS = [
    ("portal.html","PORTAL_HTML",True),
    ("favicon.png","FAVICON_PNG",False),
]


def build_tz_json(csv_path: pathlib.Path) -> bytes:
    """data/zones.csv (vendored posix_tz_db) -> compact JSON object
    {"IANA name": "POSIX string", ...}, served gzipped at /tz.json (#252).

    The head "UTC" entry maps to "" — the firmware's stored default — so
    the UI's reverse lookup round-trips a fresh device to "UTC" instead of
    some Etc/ alias."""
    table = {"UTC": ""}
    with csv_path.open(encoding="utf-8", newline="") as fh:
        for row in csv.reader(fh):
            if len(row) == 2 and row[0]:
                table[row[0]] = row[1]
    return json.dumps(table, separators=(",", ":"), ensure_ascii=False).encode("utf-8")


def build_header(project_dir: pathlib.Path) -> None:
    data_dir = project_dir / "data"
    output_header = project_dir / "WebAssets.h"

    lines_in = {}
    for filename, _, _ in ASSETS:
        lines_in[filename] = (data_dir / filename).read_bytes()

    unit_hex = data_dir / "unit-firmware.hex"
    unit_bin = unit_firmware_image(project_dir)
    tz_json = build_tz_json(data_dir / "zones.csv")
    # The page: web/ joined into one document.
    page_html = web_bundle.build_page(project_dir)

    with output_header.open("w", encoding="utf-8") as fh:
        fh.write(GENERATED_BANNER)
        fh.write("#pragma once\n\n#include <Arduino.h>\n\n")
        for filename, varname, gz in ASSETS:
            data = lines_in[filename]
            if gz:
                data = compress_asset(data)
                varname = varname + "_GZ"
            emit_array(fh, varname, data)
        emit_array(fh, "TZ_JSON_GZ", compress_asset(tz_json))
        emit_array(fh, "PAGE_HTML_GZ", compress_asset(page_html))
        emit_array(fh, "UNIT_FIRMWARE_BIN", unit_bin)

    print(f"[build_assets] wrote {output_header.name}")
    for filename, _, gz in ASSETS:
        data = lines_in[filename]
        if gz:
            print(f"  {filename:<16} gz {len(data):>5} -> {len(compress_asset(data)):>5}")
        else:
            print(f"  {filename:<16} raw {len(data):>5}")
    print(f"  tz.json          gz {len(tz_json):>5} -> {len(compress_asset(tz_json)):>5}")
    print(f"  web/ (the page)  gz {len(page_html):>5} -> {len(compress_asset(page_html)):>5}")
    print(f"  unit-firmware    hex {unit_hex.stat().st_size:>5} -> bin {len(unit_bin):>5}")


if _UNDER_SCONS:
    # The stamped copy carries the env name: ESP32 boards have no eagle
    # ldscript, so the env identifies the layout.
    _tag = write_version_header(_PROJECT_DIR, with_unit_bundle=True)
    build_header(_PROJECT_DIR)
    stamp_firmware_bin(  # noqa: F821
        env, f"firmware-{_tag}-{env['PIOENV']}.bin")  # noqa: F821
