"""PlatformIO pre-build script for the v2 rescue app (#195).

Generates:
  - RescueAssets.h: PROGMEM arrays for the rescue page + SparkMD5.
  - BuildVersion.h: #define for the current git commit.

No alphabet drift check, no favicon, no unit bundle — two assets total.
data/md5.js is a verbatim copy of Master's — the ?md5=
wire contract needs client-side hashing on the self-contained rescue page.

Invoked by PlatformIO via `extra_scripts = pre:build_assets.py`.
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
    compress_asset,
    emit_array,
    stamp_firmware_bin,
    version_header_text,
    version_tag,
    write_version_header,
)

ASSETS = [
    ("rescue.html", "RESCUE_HTML", True),
    ("md5.js",      "MD5_JS",      True),
]


def build_header(project_dir: pathlib.Path) -> None:
    data_dir = project_dir / "data"
    output_header = project_dir / "RescueAssets.h"

    with output_header.open("w", encoding="utf-8") as fh:
        fh.write(GENERATED_BANNER)
        fh.write("#pragma once\n\n#include <Arduino.h>\n\n")
        for filename, varname, gz in ASSETS:
            data = (data_dir / filename).read_bytes()
            if gz:
                gz_data = compress_asset(data)
                emit_array(fh, varname + "_GZ", gz_data)
                print(f"  {filename:<16} gz {len(data):>5} -> {len(gz_data):>5}")
            else:
                emit_array(fh, varname, data)
                print(f"  {filename:<16} raw {len(data):>5}")
    print(f"[build_assets] wrote {output_header.name}")


if _UNDER_SCONS:
    # The stamped copy (rescue-<rev>.bin) is the file POSTed to
    # /firmware/rescue.
    _tag = write_version_header(_PROJECT_DIR, with_unit_bundle=False)
    build_header(_PROJECT_DIR)
    stamp_firmware_bin(env, f"rescue-{_tag}.bin")  # noqa: F821
