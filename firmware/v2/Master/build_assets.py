"""PlatformIO pre-build script for the v2 master (#186).

Generates:
  - WebAssets.h: PROGMEM arrays for the web UI assets.
  - BuildVersion.h: #define for the current git commit (short hash +
    dirty flag), so the master firmware can report the version that
    was actually built into it.

v2 copy of firmware/v1/ESPMaster/build_assets.py. Since the reflash slice
(#205) it also bundles the unit firmware exactly like v1: data/
unit-firmware.hex (+ .rev sidecar, both committed — staged by
flashing/flasher/make_manifest.py) becomes UNIT_FIRMWARE_BIN in WebAssets.h
and BUNDLED_UNIT_REV in BuildVersion.h. The UI is served straight from
PROGMEM — no filesystem image, no uploadfs step.

Invoked by PlatformIO via `extra_scripts = pre:build_assets.py`.
"""

import csv
import gzip
import json
import pathlib
import re
import subprocess

# The Wall Console (#399) is the whole UI. The pre-#396 five-tab panel
# (index.html / script.js / style.css) was deleted with it, not kept beside
# it: two front doors would have been the hybrid the redesign set out to end.
ASSETS = [
    ("console.html",     "CONSOLE_HTML",   True),
    ("constants.js",     "CONSTANTS_JS",   True),
    ("console.css",      "CONSOLE_CSS",    True),
    ("console.js",       "CONSOLE_JS",     True),
    ("console-detail.js","CONSOLE_DETAIL_JS", True),
    ("portal.html",      "PORTAL_HTML",    True),
    ("md5.js",           "MD5_JS",         True),
    ("favicon.png",      "FAVICON_PNG",    False),
]


def parse_intel_hex(path: pathlib.Path) -> bytes:
    """Return the raw bytes produced by applying every data record of an
    Intel-HEX file, filling gaps with 0xFF. Good enough for AVR sketches
    that are contiguous from 0."""
    out = bytearray()
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line.startswith(":"):
            continue
        byte_count = int(line[1:3], 16)
        addr = int(line[3:7], 16)
        rec_type = int(line[7:9], 16)
        if rec_type == 0x00:
            data = bytes.fromhex(line[9:9 + byte_count * 2])
            while len(out) < addr + byte_count:
                out.append(0xFF)
            out[addr:addr + byte_count] = data
        elif rec_type == 0x01:
            break
    return bytes(out)


def pad_to_page(data: bytes, page: int = 128) -> bytes:
    remainder = len(data) % page
    if remainder == 0:
        return data
    return data + b"\xFF" * (page - remainder)


def bundled_unit_rev(project_dir: pathlib.Path, fallback: str) -> str:
    """Return the rev the bundled unit-firmware.hex was built at.

    Read from data/unit-firmware.rev if present (written by the
    make_manifest.py stage step). Falls back to the master's own rev so a
    missing sidecar degrades instead of blowing up (v1 #31).
    """
    sidecar = project_dir / "data" / "unit-firmware.rev"
    if sidecar.exists():
        return sidecar.read_text(encoding="utf-8").strip() or fallback
    return fallback


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


def compress_asset(data: bytes) -> bytes:
    # mtime=0 keeps the gzip MTIME header field constant so two bakes of the
    # same source are byte-identical — WebAssets.h, the firmware bin and its
    # sketchMd5 must be reproducible per commit (#168).
    return gzip.compress(data, 9, mtime=0)


def emit_array(fh, name: str, data: bytes) -> None:
    fh.write(f"const uint8_t {name}[] PROGMEM = {{\n  ")
    for i, b in enumerate(data):
        fh.write(f"0x{b:02X},")
        if (i + 1) % 16 == 0:
            fh.write("\n  ")
        else:
            fh.write(" ")
    fh.write("\n};\n")
    fh.write(f"const size_t {name}_LEN = {len(data)};\n\n")


def git_short_rev(project_dir: pathlib.Path) -> tuple[str, bool]:
    """Return (short commit hash, dirty?) for the repo that contains
    project_dir. Falls back to ("unknown", False) if git isn't available."""
    try:
        rev = subprocess.check_output(
            ["git", "rev-parse", "--short", "HEAD"],
            cwd=project_dir, stderr=subprocess.DEVNULL,
        ).decode().strip()
    except (FileNotFoundError, subprocess.CalledProcessError):
        return ("unknown", False)
    try:
        dirty = bool(subprocess.check_output(
            ["git", "status", "--porcelain"],
            cwd=project_dir, stderr=subprocess.DEVNULL,
        ).decode().strip())
    except (FileNotFoundError, subprocess.CalledProcessError):
        dirty = False
    return (rev, dirty)


def build_version_header(project_dir: pathlib.Path) -> None:
    rev, dirty = git_short_rev(project_dir)
    tag = f"{rev}-dirty" if dirty else rev
    unit_rev = bundled_unit_rev(project_dir, fallback=tag)
    output = project_dir / "BuildVersion.h"
    output.write_text(
        "// Auto-generated by build_assets.py — do not edit.\n"
        "#pragma once\n"
        f'#define GIT_REV "{tag}"\n'
        f'#define BUNDLED_UNIT_REV "{unit_rev}"\n',
        encoding="utf-8",
    )
    print(f"[build_assets] wrote {output.name}  GIT_REV={tag}  BUNDLED_UNIT_REV={unit_rev}")


def parse_header_alphabet(header_text: str) -> str:
    """Extract the SFP_ALPHABET string literal from SplitFlapProtocol.h.

    Raises ValueError if the #define is missing so a botched header edit
    fails the build loudly instead of silently skipping the drift check.
    See issue #149.
    """
    m = re.search(r'#define\s+SFP_ALPHABET\s+"((?:[^"\\]|\\.)*)"', header_text)
    if not m:
        raise ValueError("SFP_ALPHABET #define not found in SplitFlapProtocol.h")
    return m.group(1)


def parse_define(text: str, name: str) -> str:
    """Return the literal body of `#define <name> <value>`, minus any trailing
    comment. Raises ValueError when the define is missing, so a header rename
    fails the build loudly instead of silently baking a stale constant."""
    m = re.search(rf"^#define\s+{re.escape(name)}\s+(.+?)\s*(?://.*)?$",
                  text, flags=re.MULTILINE)
    if not m:
        raise ValueError(f"{name} #define not found")
    return m.group(1).strip()


def parse_int_define(text: str, name: str) -> int:
    """Same, for integer constants — including the `(1 << 4)` bit-flag form."""
    raw = parse_define(text, name)
    m = re.fullmatch(r"\(?\s*1\s*<<\s*(\d+)\s*\)?", raw)
    if m:
        return 1 << int(m.group(1))
    return int(raw, 0)


def shared_protocol_header(project_dir: pathlib.Path) -> pathlib.Path:
    """The master<->unit protocol contract, shared with the Nano unit
    firmware. Lives in firmware/v2/shared (migrated from v1 at #311) —
    matching the -I ../shared include path in platformio.ini."""
    return project_dir.parent / "shared" / "SplitFlapProtocol.h"


def console_constants(project_dir: pathlib.Path) -> dict:
    """Every firmware constant the web console needs, read from the headers
    that own them.

    The console used to hand-copy these — the alphabet most visibly — with a
    build-time gate comparing the copy to the header (#149). A gate catches
    drift; generating removes the second copy, so there is nothing to drift.
    That is the same rule #408 applied to the C headers, extended to the one
    consumer that cannot #include them.
    """
    shared = project_dir.parent / "shared"
    protocol = shared / "SplitFlapProtocol.h"
    health = (shared / "UnitHealth.h").read_text(encoding="utf-8")
    protocol_text = protocol.read_text(encoding="utf-8")
    events = (project_dir / "UnitEventLog.h").read_text(encoding="utf-8")
    ini = (project_dir / "platformio.ini").read_text(encoding="utf-8")

    alphabet = parse_header_alphabet(protocol_text)
    steps_per_revolution = parse_int_define(protocol_text, "SFP_OFFSET_LIMIT_STEPS")
    max_units = re.search(r"-D\s+UNITS_AMOUNT=(\d+)", ini)
    if not max_units:
        raise ValueError("UNITS_AMOUNT not found in platformio.ini")

    return {
        "alphabet": alphabet,
        "flapAmount": len(alphabet),
        "stepsPerRevolution": steps_per_revolution,
        "stepsPerFlap": steps_per_revolution / len(alphabet),
        "addressBase": parse_int_define(protocol_text, "SFP_I2C_ADDRESS_BASE"),
        "maxUnits": int(max_units.group(1)),
        "vccFloorMv": parse_int_define(events, "UNIT_VCC_MIN_FLOOR_MV"),
        "flag": {
            "moving": parse_int_define(health, "UNIT_FLAG_MOVING"),
            "homeFailed": parse_int_define(health, "UNIT_FLAG_LAST_HOME_FAILED"),
            "hallNever": parse_int_define(health, "UNIT_FLAG_HALL_NEVER"),
            "addrEeprom": parse_int_define(health, "UNIT_FLAG_ADDR_EEPROM"),
            "homed": parse_int_define(health, "UNIT_FLAG_HOMED"),
        },
    }


def build_constants(project_dir: pathlib.Path) -> pathlib.Path:
    """Write data/constants.js from the firmware headers. Generated, and
    gitignored like WebAssets.h and BuildVersion.h — never hand-edited."""
    out = project_dir / "data" / "constants.js"
    body = json.dumps(console_constants(project_dir), separators=(",", ":"),
                      ensure_ascii=False)
    out.write_text(
        "/* Auto-generated by build_assets.py — do not edit.\n"
        " * Every value here is read from the firmware header that owns it,\n"
        " * so the console cannot drift from the hardware it is describing. */\n"
        f"var SFP = {body};\n"
        "if (typeof module === \"object\" && module.exports) module.exports = SFP;\n",
        encoding="utf-8",
    )
    print(f"[build_assets] wrote data/{out.name} from the firmware headers")
    return out


def build_header(project_dir: pathlib.Path) -> None:
    build_constants(project_dir)

    data_dir = project_dir / "data"
    output_header = project_dir / "WebAssets.h"

    lines_in = {}
    for filename, _, _ in ASSETS:
        lines_in[filename] = (data_dir / filename).read_bytes()

    unit_hex = data_dir / "unit-firmware.hex"
    unit_bin = pad_to_page(parse_intel_hex(unit_hex))
    tz_json = build_tz_json(data_dir / "zones.csv")

    with output_header.open("w", encoding="utf-8") as fh:
        fh.write("// Auto-generated by build_assets.py — do not edit.\n")
        fh.write("#pragma once\n\n#include <Arduino.h>\n\n")
        for filename, varname, gz in ASSETS:
            data = lines_in[filename]
            if gz:
                data = compress_asset(data)
                varname = varname + "_GZ"
            emit_array(fh, varname, data)
        emit_array(fh, "TZ_JSON_GZ", compress_asset(tz_json))
        emit_array(fh, "UNIT_FIRMWARE_BIN", unit_bin)

    print(f"[build_assets] wrote {output_header.name}")
    for filename, _, gz in ASSETS:
        data = lines_in[filename]
        if gz:
            print(f"  {filename:<16} gz {len(data):>5} -> {len(compress_asset(data)):>5}")
        else:
            print(f"  {filename:<16} raw {len(data):>5}")
    print(f"  tz.json          gz {len(tz_json):>5} -> {len(compress_asset(tz_json)):>5}")
    print(f"  unit-firmware    hex {unit_hex.stat().st_size:>5} -> bin {len(unit_bin):>5}")


# PlatformIO invokes this file as a pre-build script, setting up a SCons
# env and calling Import() as a builtin. Run the build only when invoked
# that way — plain `import build_assets` (e.g. from pytest) is a no-op.
try:
    Import("env")  # noqa: F821  (provided by PlatformIO SCons env)
    _project_dir = pathlib.Path(env["PROJECT_DIR"])  # noqa: F821
    build_version_header(_project_dir)
    build_header(_project_dir)

    # Post-build: drop a copy of firmware.bin next to itself with the git
    # rev AND env name in the filename so shipping / archiving is
    # self-describing (v1 used the flash-size label; ESP32 boards have no
    # eagle ldscript, so the env name carries the layout identity here).
    _rev, _dirty = git_short_rev(_project_dir)
    _tag = f"{_rev}-dirty" if _dirty else _rev
    _env_name = env["PIOENV"]  # noqa: F821

    def _stamp_firmware_filename(source, target, env):  # noqa: F821
        import shutil
        bin_path = pathlib.Path(str(target[0]))
        stamped = bin_path.with_name(f"firmware-{_tag}-{_env_name}.bin")
        shutil.copyfile(bin_path, stamped)
        print(f"[build_assets] copied {bin_path.name} -> {stamped.name}")

    env.AddPostAction(  # noqa: F821
        "$BUILD_DIR/${PROGNAME}.bin", _stamp_firmware_filename
    )
except NameError:
    pass
