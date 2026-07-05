# Universal Flasher (`split-flap-flasher.exe`) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** One Windows exe that provisions a complete stock display from zero — builds the ArduinoISP programmer, flashes twiboot to N Nanos with verification, flashes the master, guides every wire, and verifies the live display — per `docs/superpowers/specs/2026-07-05-universal-flasher-design.md` (issue #124).

**Architecture:** Python package `flashing/flasher/` (small modules, pure logic separated from subprocess/HTTP so it's pytest-able on Linux). PyInstaller one-file exe built by a GitHub Actions `windows-latest` job that also builds the firmware in the load-bearing order (Unit → stage hex+rev into ESPMaster/data → ESPMaster) and enforces a manifest consistency gate. Dev mode runs `python -m flasher` against assets staged by `make_manifest.py`.

**Tech Stack:** Python ≥3.11, pyserial + esptool (only runtime deps), avrdude (bundled exe on Windows / system binary in dev), pytest, PyInstaller, GitHub Actions.

## Global Constraints

- Runtime deps: **stdlib + pyserial + esptool only**. No colorama, no click, no requests.
- All pure logic must be importable and tested without hardware, network, or Windows.
- Fuses (from the retired `.bat`, unchanged): **LFUSE 0xFF, HFUSE 0xDC, EFUSE 0xFD**. EFUSE on m328p reads back with undefined upper bits — compare only `& 0x07`.
- ICSP programmer settings (unchanged): `-c stk500v1 -b 19200 -p m328p`.
- ESP flash: image at `0x0`, `--before no_reset --after no_reset`, baud 115200, chip esp8266. Header gate: magic `0xE9`, flash-size nibble `0x2` (1 MB) fatal check; non-DOUT mode = warning.
- OTA verdicts and retry policy copied exactly from `flashing/ota-master.sh` (retry **only** `reverted`; quiet-OTA `POST /firmware/ota-mode` before every attempt when enabled).
- Unit count asked at runtime, 1–16. DIP for unit N = `format(N-1, "04b")` (matches README table: unit 1 → `0000` → I2C 0x01).
- Conventional commits, one commit per task, reference #124. No version bump.
- Files ≤400 lines. Tests live in `flashing/flasher/tests/`; run from `flashing/`: `python -m pytest flasher/tests/ -v`.
- Windows-only UX; code must still *run* on Linux for dev (avrdude via PATH).

## File Structure

```
flashing/
├── flasher/
│   ├── __init__.py          (empty)
│   ├── __main__.py          (from flasher.main import main; main())
│   ├── main.py              menu loop + dispatch
│   ├── wizard.py            provisioning flow + network_verdict()
│   ├── ports.py             pyserial enumeration, new-port watching, driver hint
│   ├── avr.py               avrdude arg builders + output parsers + runners
│   ├── esp.py               image header validation + esptool invocation
│   ├── ota.py               verdict logic, multipart upload, /settings client
│   ├── assets.py            asset/manifest resolution (frozen vs dev)
│   ├── session.py           resumable provisioning state
│   ├── wiring.py            ASCII diagrams + DIP rendering (pure)
│   ├── ui.py                prompts/colors/ANSI enable
│   ├── make_manifest.py     stage/collect/gate build script (CI + dev)
│   ├── flasher.spec         PyInstaller spec
│   ├── assets/              committed: ArduinoISP.hex + LICENSES.md; build-time: firmware + avrdude + manifest.json (gitignored)
│   ├── assets-src/arduinoisp/   vendored ArduinoISP PIO project
│   └── tests/               test_*.py per module
└── (old .bat/.sh files deleted in Task 14)
```

---

### Task 1: Package skeleton + `assets.py` (manifest validation)

**Files:**
- Create: `flashing/flasher/__init__.py`, `flashing/flasher/__main__.py`, `flashing/flasher/assets.py`, `flashing/flasher/tests/__init__.py`
- Create: `flashing/.gitignore` (assets build outputs)
- Test: `flashing/flasher/tests/test_assets.py`

**Interfaces:**
- Produces: `asset_root() -> Path`, `asset_path(name: str) -> Path`, `load_manifest() -> dict` (raises `ManifestError`), `validate_manifest(manifest: dict, root: Path) -> None` (raises `ManifestError`), `REQUIRED_ASSETS: tuple[str, ...]`, `is_frozen() -> bool`.

- [ ] **Step 1: Write the failing tests**

```python
# flashing/flasher/tests/test_assets.py
import hashlib
import json
import pytest
from flasher.assets import ManifestError, validate_manifest, REQUIRED_ASSETS


def make_assets(tmp_path, names):
    manifest = {"git_rev": "abc1234", "build_date": "2026-07-05", "assets": {}}
    for name in names:
        p = tmp_path / name
        p.write_bytes(b"data-" + name.encode())
        manifest["assets"][name] = hashlib.sha256(p.read_bytes()).hexdigest()
    return manifest


def test_valid_manifest_passes(tmp_path):
    manifest = make_assets(tmp_path, REQUIRED_ASSETS)
    validate_manifest(manifest, tmp_path)  # no raise


def test_missing_required_asset_rejected(tmp_path):
    manifest = make_assets(tmp_path, REQUIRED_ASSETS[1:])
    with pytest.raises(ManifestError, match=REQUIRED_ASSETS[0]):
        validate_manifest(manifest, tmp_path)


def test_hash_mismatch_rejected(tmp_path):
    manifest = make_assets(tmp_path, REQUIRED_ASSETS)
    (tmp_path / REQUIRED_ASSETS[0]).write_bytes(b"tampered")
    with pytest.raises(ManifestError, match="hash mismatch"):
        validate_manifest(manifest, tmp_path)


def test_listed_file_absent_rejected(tmp_path):
    manifest = make_assets(tmp_path, REQUIRED_ASSETS)
    manifest["assets"]["ghost.bin"] = "0" * 64
    with pytest.raises(ManifestError, match="ghost.bin"):
        validate_manifest(manifest, tmp_path)


def test_missing_keys_rejected(tmp_path):
    with pytest.raises(ManifestError, match="git_rev"):
        validate_manifest({"assets": {}}, tmp_path)
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd /home/lucas/split-flap/flashing && python -m pytest flasher/tests/test_assets.py -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'flasher'` (or ImportError for names).

- [ ] **Step 3: Implement**

```python
# flashing/flasher/assets.py
"""Bundled-asset + manifest resolution.

Frozen (PyInstaller): assets live under sys._MEIPASS/assets.
Dev: assets live in flashing/flasher/assets/, staged by make_manifest.py.
The manifest is the anti-drift contract: the tool refuses to run when any
asset is missing or its SHA-256 disagrees.
"""
import hashlib
import json
import sys
from pathlib import Path


class ManifestError(Exception):
    pass


REQUIRED_ASSETS = (
    "ArduinoISP.hex",
    "twiboot-atmega328p-16mhz.hex",
    "master-firmware.bin",
    "unit-firmware.hex",
)


def is_frozen() -> bool:
    return getattr(sys, "frozen", False)


def asset_root() -> Path:
    if is_frozen():
        return Path(sys._MEIPASS) / "assets"
    return Path(__file__).resolve().parent / "assets"


def asset_path(name: str) -> Path:
    return asset_root() / name


def validate_manifest(manifest: dict, root: Path) -> None:
    for key in ("git_rev", "build_date", "assets"):
        if key not in manifest:
            raise ManifestError(f"manifest missing key: {key}")
    for name in REQUIRED_ASSETS:
        if name not in manifest["assets"]:
            raise ManifestError(f"manifest missing required asset: {name}")
    for name, expected in manifest["assets"].items():
        p = root / name
        if not p.is_file():
            raise ManifestError(f"asset listed but absent: {name}")
        actual = hashlib.sha256(p.read_bytes()).hexdigest()
        if actual != expected:
            raise ManifestError(f"asset hash mismatch: {name}")


def load_manifest() -> dict:
    root = asset_root()
    mf = root / "manifest.json"
    if not mf.is_file():
        raise ManifestError(
            f"no manifest.json in {root} — dev: run "
            "'python flasher/make_manifest.py collect' after building firmware"
        )
    manifest = json.loads(mf.read_text())
    validate_manifest(manifest, root)
    return manifest
```

```python
# flashing/flasher/__init__.py
```

```python
# flashing/flasher/__main__.py
from flasher.main import main

main()
```

(`flasher/main.py` arrives in Task 9; `__main__.py` import failing until then is fine — tests import modules directly.)

```gitignore
# flashing/.gitignore
flasher/assets/manifest.json
flasher/assets/master-firmware.bin
flasher/assets/unit-firmware.hex
flasher/assets/twiboot-atmega328p-16mhz.hex
flasher/assets/avrdude/
*.session.json
```

(`ArduinoISP.hex` and `LICENSES.md` are the only committed assets; firmware assets are staged per build.)

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd /home/lucas/split-flap/flashing && python -m pytest flasher/tests/test_assets.py -v`
Expected: 5 passed.

- [ ] **Step 5: Commit**

```bash
git add flashing/flasher flashing/.gitignore
git commit -m "feat(flashing): flasher package skeleton + manifest validation (#124)"
```

---

### Task 2: `wiring.py` — DIP rendering + diagrams

**Files:**
- Create: `flashing/flasher/wiring.py`
- Test: `flashing/flasher/tests/test_wiring.py`

**Interfaces:**
- Produces: `dip_pattern(unit_no: int) -> str` (raises `ValueError` outside 1..16), `dip_visual(unit_no: int) -> str`, `DIAGRAMS: dict[str, str]` with keys `"programmer"`, `"icsp"`, `"esp_uart"`, `"assembly"`.

- [ ] **Step 1: Write the failing tests**

```python
# flashing/flasher/tests/test_wiring.py
import pytest
from flasher.wiring import DIAGRAMS, dip_pattern, dip_visual


@pytest.mark.parametrize("unit,expected", [(1, "0000"), (2, "0001"), (3, "0010"), (10, "1001"), (16, "1111")])
def test_dip_pattern_matches_readme_table(unit, expected):
    assert dip_pattern(unit) == expected


@pytest.mark.parametrize("bad", [0, 17, -1])
def test_dip_pattern_rejects_out_of_range(bad):
    with pytest.raises(ValueError):
        dip_pattern(bad)


def test_dip_visual_marks_up_switches():
    v = dip_visual(3)  # 0010 -> SW3 up
    assert "0010" in v and "up" in v


def test_all_diagrams_present_and_substantial():
    for key in ("programmer", "icsp", "esp_uart", "assembly"):
        assert key in DIAGRAMS and len(DIAGRAMS[key]) > 100


def test_key_wiring_facts():
    assert "D10" in DIAGRAMS["icsp"]
    assert "10uF" in DIAGRAMS["programmer"] or "10 µF" in DIAGRAMS["programmer"]
    assert "3.3V" in DIAGRAMS["esp_uart"]
    assert "GPIO0" in DIAGRAMS["esp_uart"]
    assert "SDA" in DIAGRAMS["assembly"]
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd /home/lucas/split-flap/flashing && python -m pytest flasher/tests/test_wiring.py -v`
Expected: FAIL — ImportError.

- [ ] **Step 3: Implement**

```python
# flashing/flasher/wiring.py
"""Pin-by-pin wiring diagrams shown inline at every hardware step.

The exe is the single source of connection truth at the bench — no README,
no browser. Facts sourced from flashing/README.md + UnitBootloader/README.md.
"""


def dip_pattern(unit_no: int) -> str:
    if not 1 <= unit_no <= 16:
        raise ValueError(f"unit number out of range 1..16: {unit_no}")
    return format(unit_no - 1, "04b")


def dip_visual(unit_no: int) -> str:
    bits = dip_pattern(unit_no)
    switches = "  ".join(
        f"SW{i + 1}:{'up' if b == '1' else 'down'}" for i, b in enumerate(bits)
    )
    return f"DIP {bits}  ({switches})   [1 = up]"


DIAGRAMS = {
    "programmer": """\
BUILD THE PROGRAMMER (one-time)
A spare Uno or Nano becomes an Arduino-as-ISP programmer.

  1. Plug the spare board into USB. This tool flashes ArduinoISP.hex
     onto it through its normal serial bootloader — nothing to wire yet.
  2. AFTER that flash succeeds, fit a 10uF capacitor between the
     PROGRAMMER's RESET pin and GND (stripe/minus leg to GND).
     This stops the programmer auto-resetting when avrdude opens the port.
  3. Leave it on USB — it now programs the target Nanos via 6 wires.
""",
    "icsp": """\
PROGRAMMER -> TARGET NANO (6 wires, repeat per unit)

  programmer D10  ->  target RST
  programmer D11  ->  target D11 (MOSI)
  programmer D12  ->  target D12 (MISO)
  programmer D13  ->  target D13 (SCK)
  programmer 5V   ->  target 5V
  programmer GND  ->  target GND

  ! DISCONNECT THE STEPPER from the unit PCB while flashing —
    it loads the SPI pins and causes 0xFFFFFF signature errors.
  ! MOSI/MISO swapped is the classic wiring mistake.
""",
    "esp_uart": """\
USB-UART ADAPTER -> ESP-01 (master flash)

  adapter 3.3V  ->  ESP VCC        (NEVER 5V — kills the ESP)
  adapter 3.3V  ->  ESP CH_PD/EN
  adapter GND   ->  ESP GND
  adapter TX    ->  ESP RX         (crossed!)
  adapter RX    ->  ESP TX
  ESP GPIO0     ->  GND            (programming mode, only during flash)

  Flash sequence: jumper GPIO0 to GND -> plug in adapter -> flash ->
  unplug -> REMOVE the GPIO0 jumper -> plug back in (normal boot).
  Cheap adapters can't power the ESP — if flashing is flaky, feed
  VCC from a separate 3.3V supply and share GND.
""",
    "assembly": """\
DISPLAY ASSEMBLY (before the master's first normal boot)

  - One shared 5V rail powers all Nanos and the ESP-01 (via its 3.3V reg).
  - I2C bus: ESP-01 GPIO1 -> SDA of every Nano, GPIO3 -> SCL of every Nano.
  - Every Nano: DIP set (contiguous from unit 1 = 0000), twiboot installed.
  - Power EVERYTHING before booting the master: on its first boot scan the
    master auto-installs the unit firmware over I2C to every Nano it sees
    in bootloader mode. Units missing from the bus miss that window (they
    get picked up on a later reboot, but verify reads cleaner first time).
""",
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd /home/lucas/split-flap/flashing && python -m pytest flasher/tests/test_wiring.py -v`
Expected: all passed.

- [ ] **Step 5: Commit**

```bash
git add flashing/flasher/wiring.py flashing/flasher/tests/test_wiring.py
git commit -m "feat(flashing): wiring diagrams + DIP rendering (#124)"
```

---

### Task 3: `avr.py` — arg builders, signature + fuse parsing

**Files:**
- Create: `flashing/flasher/avr.py`
- Test: `flashing/flasher/tests/test_avr.py`

**Interfaces:**
- Consumes: `assets.asset_path`, `assets.is_frozen`.
- Produces (pure): `arduinoisp_args(avrdude, conf, port, baud, hex_path) -> list[str]`, `icsp_flash_args(avrdude, conf, port, hex_path) -> list[str]`, `probe_args(avrdude, conf, port) -> list[str]`, `fuse_write_args(avrdude, conf, port) -> list[str]`, `fuse_read_args(avrdude, conf, port) -> list[str]`, `parse_signature(output: str) -> str | None`, `parse_fuses(output: str) -> tuple[int, int, int] | None`, `fuses_ok(lfuse: int, hfuse: int, efuse: int) -> bool`, constants `EXPECTED_SIGNATURE = "1e950f"`, `LFUSE, HFUSE, EFUSE`.
- Produces (impure): `find_avrdude() -> tuple[str, str | None]` (exe path, conf path or None; raises `AvrdudeNotFound` with install hint), `run_avrdude(args: list[str]) -> tuple[int, str]` (returncode, combined output).

- [ ] **Step 1: Write the failing tests**

```python
# flashing/flasher/tests/test_avr.py
from flasher.avr import (
    EXPECTED_SIGNATURE, arduinoisp_args, fuse_read_args, fuse_write_args,
    fuses_ok, icsp_flash_args, parse_fuses, parse_signature, probe_args,
)


def test_icsp_args_use_stk500v1_at_19200():
    args = icsp_flash_args("avrdude", "avrdude.conf", "COM4", "twiboot.hex")
    joined = " ".join(args)
    assert "-c stk500v1" in joined and "-b 19200" in joined and "-p m328p" in joined
    assert "flash:w:twiboot.hex:i" in joined


def test_arduinoisp_args_use_serial_bootloader():
    args = arduinoisp_args("avrdude", "avrdude.conf", "COM4", 115200, "ArduinoISP.hex")
    joined = " ".join(args)
    assert "-c arduino" in joined and "-b 115200" in joined
    assert "flash:w:ArduinoISP.hex:i" in joined


def test_probe_args_have_no_write_operations():
    joined = " ".join(probe_args("avrdude", "avrdude.conf", "COM4"))
    assert ":w:" not in joined


def test_fuse_write_args_match_retired_bat():
    joined = " ".join(fuse_write_args("avrdude", "avrdude.conf", "COM4"))
    assert "lfuse:w:0xff:m" in joined
    assert "hfuse:w:0xdc:m" in joined
    assert "efuse:w:0xfd:m" in joined


def test_parse_signature():
    out = "avrdude: Device signature = 0x1e950f (probably m328p)"
    assert parse_signature(out) == "1e950f"
    assert parse_signature("avrdude: Device signature = 0xffffff") == "ffffff"
    assert parse_signature("no signature here") is None


def test_parse_fuses_hex_lines():
    # avrdude -U lfuse:r:-:h ... prints one value per line on stdout
    assert parse_fuses("0xff\n0xdc\n0xfd\n") == (0xFF, 0xDC, 0xFD)
    assert parse_fuses("0xff\n0xdc\n0x5\n") == (0xFF, 0xDC, 0x05)
    assert parse_fuses("garbage") is None


def test_fuses_ok_masks_efuse_undefined_bits():
    assert fuses_ok(0xFF, 0xDC, 0xFD)
    assert fuses_ok(0xFF, 0xDC, 0x05)  # same low 3 bits — m328p upper bits undefined
    assert not fuses_ok(0xFF, 0xDC, 0x04)
    assert not fuses_ok(0xFE, 0xDC, 0xFD)
    assert not fuses_ok(0xFF, 0xDA, 0xFD)


def test_signature_constant():
    assert EXPECTED_SIGNATURE == "1e950f"
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd /home/lucas/split-flap/flashing && python -m pytest flasher/tests/test_avr.py -v`
Expected: FAIL — ImportError.

- [ ] **Step 3: Implement**

```python
# flashing/flasher/avr.py
"""avrdude wrapper: pure arg builders + output parsers, thin runner.

Two programmer modes:
- serial bootloader (-c arduino) — flashing ArduinoISP.hex onto the spare
  board that becomes the programmer (115200 new bootloaders, 57600 old).
- Arduino-as-ISP (-c stk500v1 -b 19200) — everything aimed at target Nanos.
"""
import re
import shutil
import subprocess
import sys
from pathlib import Path

from flasher.assets import asset_root, is_frozen

EXPECTED_SIGNATURE = "1e950f"  # ATmega328P
LFUSE, HFUSE, EFUSE = 0xFF, 0xDC, 0xFD
_EFUSE_MASK = 0x07  # m328p efuse upper 5 bits are undefined on read


class AvrdudeNotFound(Exception):
    pass


def _base(avrdude: str, conf: str | None, port: str) -> list[str]:
    args = [avrdude]
    if conf:
        args += ["-C", conf]
    return args + ["-c", "stk500v1", "-P", port, "-b", "19200", "-p", "m328p"]


def arduinoisp_args(avrdude: str, conf: str | None, port: str, baud: int, hex_path: str) -> list[str]:
    args = [avrdude]
    if conf:
        args += ["-C", conf]
    return args + ["-c", "arduino", "-P", port, "-b", str(baud), "-p", "m328p",
                   "-D", "-U", f"flash:w:{hex_path}:i"]


def probe_args(avrdude: str, conf: str | None, port: str) -> list[str]:
    return _base(avrdude, conf, port)


def icsp_flash_args(avrdude: str, conf: str | None, port: str, hex_path: str) -> list[str]:
    return _base(avrdude, conf, port) + ["-U", f"flash:w:{hex_path}:i"]


def fuse_write_args(avrdude: str, conf: str | None, port: str) -> list[str]:
    return _base(avrdude, conf, port) + [
        "-U", f"lfuse:w:{LFUSE:#04x}:m",
        "-U", f"hfuse:w:{HFUSE:#04x}:m",
        "-U", f"efuse:w:{EFUSE:#04x}:m",
    ]


def fuse_read_args(avrdude: str, conf: str | None, port: str) -> list[str]:
    return _base(avrdude, conf, port) + [
        "-U", "lfuse:r:-:h", "-U", "hfuse:r:-:h", "-U", "efuse:r:-:h",
    ]


def parse_signature(output: str) -> str | None:
    m = re.search(r"Device signature\s*=\s*0x([0-9a-fA-F]{6})", output)
    return m.group(1).lower() if m else None


def parse_fuses(output: str) -> tuple[int, int, int] | None:
    values = re.findall(r"^0x[0-9a-fA-F]{1,2}$", output, re.MULTILINE)
    if len(values) < 3:
        return None
    l, h, e = (int(v, 16) for v in values[:3])
    return (l, h, e)


def fuses_ok(lfuse: int, hfuse: int, efuse: int) -> bool:
    return (lfuse == LFUSE and hfuse == HFUSE
            and (efuse & _EFUSE_MASK) == (EFUSE & _EFUSE_MASK))


def find_avrdude() -> tuple[str, str | None]:
    if is_frozen():
        exe = asset_root() / "avrdude" / "avrdude.exe"
        conf = asset_root() / "avrdude" / "avrdude.conf"
        if exe.is_file():
            return (str(exe), str(conf) if conf.is_file() else None)
        raise AvrdudeNotFound("bundled avrdude missing — corrupt build")
    found = shutil.which("avrdude")
    if found:
        return (found, None)
    pio = Path.home() / ".platformio" / "packages" / "tool-avrdude"
    exe = pio / ("avrdude.exe" if sys.platform == "win32" else "avrdude")
    if exe.is_file():
        return (str(exe), str(pio / "avrdude.conf"))
    raise AvrdudeNotFound(
        "avrdude not found — install it (apt install avrdude / PlatformIO) or use the exe build"
    )


def run_avrdude(args: list[str]) -> tuple[int, str]:
    proc = subprocess.run(args, capture_output=True, text=True, timeout=120)
    return (proc.returncode, proc.stdout + proc.stderr)
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd /home/lucas/split-flap/flashing && python -m pytest flasher/tests/test_avr.py -v`
Expected: all passed.

- [ ] **Step 5: Commit**

```bash
git add flashing/flasher/avr.py flashing/flasher/tests/test_avr.py
git commit -m "feat(flashing): avrdude arg builders + signature/fuse parsing (#124)"
```

---

### Task 4: `esp.py` — image header validation + esptool invocation

**Files:**
- Create: `flashing/flasher/esp.py`
- Test: `flashing/flasher/tests/test_esp.py`

**Interfaces:**
- Produces: `validate_image_header(header: bytes) -> list[str]` (returns warnings; raises `ImageError` on fatal), `flash_master(port: str, bin_path: str) -> None` (raises `ImageError` / esptool errors; runs `python -m esptool` in dev, `esptool.main()` frozen).

- [ ] **Step 1: Write the failing tests**

```python
# flashing/flasher/tests/test_esp.py
import pytest
from flasher.esp import ImageError, validate_image_header


def hdr(magic=0xE9, mode=0x03, size_freq=0x20):
    return bytes([magic, 0x02, mode, size_freq])


def test_valid_1mb_dout_header_no_warnings():
    assert validate_image_header(hdr()) == []


def test_bad_magic_fatal():
    with pytest.raises(ImageError, match="magic"):
        validate_image_header(hdr(magic=0xEA))


def test_4mb_header_fatal():
    # size nibble 0x4 = 4MB — would trip the OTA flash-config-mismatch path later
    with pytest.raises(ImageError, match="1 MB"):
        validate_image_header(hdr(size_freq=0x40))


def test_512kb_header_fatal():
    with pytest.raises(ImageError, match="1 MB"):
        validate_image_header(hdr(size_freq=0x00))


def test_non_dout_mode_is_warning_not_fatal():
    warnings = validate_image_header(hdr(mode=0x00))  # QIO
    assert len(warnings) == 1 and "DOUT" in warnings[0]


def test_truncated_header_fatal():
    with pytest.raises(ImageError, match="short"):
        validate_image_header(b"\xe9\x02")
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd /home/lucas/split-flap/flashing && python -m pytest flasher/tests/test_esp.py -v`
Expected: FAIL — ImportError.

- [ ] **Step 3: Implement**

```python
# flashing/flasher/esp.py
"""ESP-01 master flashing: image header gate + esptool invocation.

The header gate protects the OTA path: a firmware built for a bigger chip
flashes fine over USB but then rejects every future OTA with
flash-config-mismatch (#92/#94). Refuse it here, at the bench.
"""
import subprocess
import sys
from pathlib import Path

_MAGIC = 0xE9
_SIZE_NIBBLE_1MB = 0x2
_MODE_DOUT = 0x3
_MODES = {0x0: "QIO", 0x1: "QOUT", 0x2: "DIO", 0x3: "DOUT"}


class ImageError(Exception):
    pass


def validate_image_header(header: bytes) -> list[str]:
    if len(header) < 4:
        raise ImageError("image too short to contain an esp8266 header")
    if header[0] != _MAGIC:
        raise ImageError(f"not an esp8266 image (magic {header[0]:#04x}, expected 0xe9)")
    size_nibble = header[3] >> 4
    if size_nibble != _SIZE_NIBBLE_1MB:
        raise ImageError(
            f"flash-size header nibble {size_nibble:#x} is not the 1 MB ESP-01 build "
            "(esp01_1m) — flashing this would break all future OTA (#92/#94)"
        )
    warnings = []
    if header[2] != _MODE_DOUT:
        mode = _MODES.get(header[2], f"{header[2]:#04x}")
        warnings.append(f"flash mode is {mode}, expected DOUT — flash may not boot on ESP-01")
    return warnings


def flash_master(port: str, bin_path: str) -> list[str]:
    data = Path(bin_path).read_bytes()
    warnings = validate_image_header(data[:4])
    args = [
        "--chip", "esp8266", "--port", port, "--baud", "115200",
        "--before", "no_reset", "--after", "no_reset",
        "write_flash", "0x0", bin_path,
    ]
    if getattr(sys, "frozen", False):
        import esptool
        esptool.main(args)
    else:
        subprocess.run([sys.executable, "-m", "esptool"] + args, check=True)
    return warnings
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd /home/lucas/split-flap/flashing && python -m pytest flasher/tests/test_esp.py -v`
Expected: all passed.

- [ ] **Step 5: Commit**

```bash
git add flashing/flasher/esp.py flashing/flasher/tests/test_esp.py
git commit -m "feat(flashing): esp image header gate + esptool wrapper (#124)"
```

---

### Task 5: `ota.py` part 1 — verdict classification (port of ota-master.sh decision matrix)

**Files:**
- Create: `flashing/flasher/ota.py`
- Test: `flashing/flasher/tests/test_ota.py`

**Interfaces:**
- Produces: verdict string constants `SUCCESS, REVERTED, NO_HANDLER, FLASH_CONFIG_MISMATCH, UPLOAD_FAILED, UNREACHABLE, INCONSISTENT`; `RETRYABLE = {REVERTED}`; `classify_upload(http_code: int, body: str) -> str | None` (None = proceed to post-flash polling); `classify_post_flash(uploaded_md5: str, pre_md5: str, post_md5: str, last_result: str) -> str`.

- [ ] **Step 1: Write the failing tests** (each case mirrors a line of `ota-master.sh:175-224`)

```python
# flashing/flasher/tests/test_ota.py
from flasher.ota import (
    FLASH_CONFIG_MISMATCH, INCONSISTENT, NO_HANDLER, RETRYABLE, REVERTED,
    SUCCESS, UPLOAD_FAILED, classify_post_flash, classify_upload,
)

MD5 = "aabbccdd" * 4
OLD = "11223344" * 4


def test_flash_config_in_body_wins_over_http_code():
    assert classify_upload(200, "Flash config wrong") == FLASH_CONFIG_MISMATCH
    assert classify_upload(412, "flash config exceeds chip") == FLASH_CONFIG_MISMATCH


def test_non_200_is_upload_failed():
    assert classify_upload(500, "boom") == UPLOAD_FAILED
    assert classify_upload(0, "") == UPLOAD_FAILED


def test_200_clean_body_proceeds():
    assert classify_upload(200, "OK") is None


def test_md5_match_is_success_even_if_flag_disagrees():
    # sketchMd5 is the primary signal (#118) — pre-#118 firmware false-reports
    assert classify_post_flash(MD5, OLD, MD5, "reverted") == SUCCESS
    assert classify_post_flash(MD5, OLD, MD5, "ok") == SUCCESS
    assert classify_post_flash(MD5, OLD, MD5, "") == SUCCESS


def test_reverted_flag_without_md5_match():
    assert classify_post_flash(MD5, OLD, OLD, "reverted") == REVERTED


def test_no_handler_needs_empty_flag_and_unchanged_md5():
    assert classify_post_flash(MD5, OLD, OLD, "") == NO_HANDLER


def test_unreachable_post_md5_is_inconsistent():
    assert classify_post_flash(MD5, OLD, "?", "?") == INCONSISTENT


def test_changed_but_wrong_md5_is_inconsistent():
    assert classify_post_flash(MD5, OLD, "99" * 16, "ok") == INCONSISTENT


def test_only_reverted_is_retryable():
    assert RETRYABLE == {REVERTED}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd /home/lucas/split-flap/flashing && python -m pytest flasher/tests/test_ota.py -v`
Expected: FAIL — ImportError.

- [ ] **Step 3: Implement** (classification only; upload/polling arrives Task 8)

```python
# flashing/flasher/ota.py
"""OTA upload + verdicts — Python port of flashing/ota-master.sh.

Decision matrix ported line-for-line (ota-master.sh:175-224, #53/#60/#118):
the running sketchMd5 matching the uploaded file's MD5 is the primary
success signal; the lastFlashResult flag is secondary. Retry policy:
ONLY 'reverted' retries — every other verdict is a config/network/logic
problem that retrying cannot fix.
"""

SUCCESS = "success"
REVERTED = "reverted"
NO_HANDLER = "no-handler"
FLASH_CONFIG_MISMATCH = "flash-config-mismatch"
UPLOAD_FAILED = "upload-failed"
UNREACHABLE = "unreachable"
INCONSISTENT = "inconsistent"

RETRYABLE = {REVERTED}


def classify_upload(http_code: int, body: str) -> str | None:
    if "flash config" in body.lower():
        return FLASH_CONFIG_MISMATCH
    if http_code != 200:
        return UPLOAD_FAILED
    return None


def classify_post_flash(uploaded_md5: str, pre_md5: str, post_md5: str, last_result: str) -> str:
    if post_md5 == uploaded_md5:
        return SUCCESS
    if last_result == "reverted":
        return REVERTED
    if last_result == "" and post_md5 == pre_md5 and post_md5 != "?":
        return NO_HANDLER
    return INCONSISTENT
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd /home/lucas/split-flap/flashing && python -m pytest flasher/tests/test_ota.py -v`
Expected: all passed.

- [ ] **Step 5: Commit**

```bash
git add flashing/flasher/ota.py flashing/flasher/tests/test_ota.py
git commit -m "feat(flashing): OTA verdict classification ported from ota-master.sh (#124)"
```

---

### Task 6: `session.py` — resumable provisioning state

**Files:**
- Create: `flashing/flasher/session.py`
- Test: `flashing/flasher/tests/test_session.py`

**Interfaces:**
- Produces: `@dataclass Session(unit_count: int = 0, done: list[int] = ..., skipped: list[int] = ..., programmer_port: str | None = None)`, `next_unit(s: Session) -> int | None` (lowest unit not in `done`; skipped units come back AFTER untouched ones), `load_session(path: Path) -> Session | None` (None on missing/corrupt), `save_session(s: Session, path: Path) -> None`, `default_session_path() -> Path` (exe dir when frozen, cwd otherwise, named `split-flap-flasher.session.json`).

- [ ] **Step 1: Write the failing tests**

```python
# flashing/flasher/tests/test_session.py
from pathlib import Path
from flasher.session import Session, load_session, next_unit, save_session


def test_next_unit_walks_in_order():
    s = Session(unit_count=3)
    assert next_unit(s) == 1
    s.done.append(1)
    assert next_unit(s) == 2


def test_skipped_units_revisited_after_untouched_ones():
    s = Session(unit_count=3, done=[1], skipped=[2])
    assert next_unit(s) == 3          # untouched first
    s.done.append(3)
    assert next_unit(s) == 2          # then the skipped one
    s.done.append(2)
    assert next_unit(s) is None       # complete (even though still in skipped list)


def test_roundtrip(tmp_path):
    p = tmp_path / "s.json"
    save_session(Session(unit_count=12, done=[1, 2], skipped=[3], programmer_port="COM4"), p)
    s = load_session(p)
    assert s.unit_count == 12 and s.done == [1, 2] and s.skipped == [3]
    assert s.programmer_port == "COM4"


def test_load_missing_or_corrupt_returns_none(tmp_path):
    assert load_session(tmp_path / "nope.json") is None
    bad = tmp_path / "bad.json"
    bad.write_text("{not json")
    assert load_session(bad) is None
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd /home/lucas/split-flap/flashing && python -m pytest flasher/tests/test_session.py -v`
Expected: FAIL — ImportError.

- [ ] **Step 3: Implement**

```python
# flashing/flasher/session.py
"""Resumable provisioning state — unit 9/12 on Tuesday resumes Wednesday."""
import json
import sys
from dataclasses import asdict, dataclass, field
from pathlib import Path


@dataclass
class Session:
    unit_count: int = 0
    done: list = field(default_factory=list)
    skipped: list = field(default_factory=list)
    programmer_port: str | None = None


def next_unit(s: Session) -> int | None:
    untouched = [n for n in range(1, s.unit_count + 1)
                 if n not in s.done and n not in s.skipped]
    if untouched:
        return untouched[0]
    revisit = [n for n in s.skipped if n not in s.done]
    return revisit[0] if revisit else None


def default_session_path() -> Path:
    base = Path(sys.executable).parent if getattr(sys, "frozen", False) else Path.cwd()
    return base / "split-flap-flasher.session.json"


def load_session(path: Path) -> Session | None:
    try:
        d = json.loads(path.read_text())
        return Session(**d)
    except (OSError, ValueError, TypeError):
        return None


def save_session(s: Session, path: Path) -> None:
    path.write_text(json.dumps(asdict(s), indent=2))
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd /home/lucas/split-flap/flashing && python -m pytest flasher/tests/test_session.py -v`
Expected: all passed.

- [ ] **Step 5: Commit**

```bash
git add flashing/flasher/session.py flashing/flasher/tests/test_session.py
git commit -m "feat(flashing): resumable provisioning session state (#124)"
```

---

### Task 7: `ports.py` — serial port enumeration + new-port watching

**Files:**
- Create: `flashing/flasher/ports.py`
- Test: `flashing/flasher/tests/test_ports.py`

**Interfaces:**
- Produces: `diff_new(before: set[str], now: set[str]) -> set[str]` (pure), `list_port_names() -> list[str]`, `describe_ports() -> list[str]` ("COM5 — USB-SERIAL CH340"), `wait_for_new_port(before: set[str], timeout: float = 60.0, poll: float = 1.0) -> str | None`, `DRIVER_HINT: str` (CH340/CP210x instructions).

- [ ] **Step 1: Write the failing tests**

```python
# flashing/flasher/tests/test_ports.py
from unittest.mock import patch
from flasher.ports import DRIVER_HINT, diff_new, wait_for_new_port


def test_diff_new():
    assert diff_new({"COM3"}, {"COM3", "COM5"}) == {"COM5"}
    assert diff_new({"COM3"}, {"COM3"}) == set()
    assert diff_new({"COM3"}, set()) == set()


def test_wait_for_new_port_returns_first_appearance():
    seq = [["COM3"], ["COM3"], ["COM3", "COM7"]]
    with patch("flasher.ports.list_port_names", side_effect=seq):
        with patch("flasher.ports.time.sleep"):
            assert wait_for_new_port({"COM3"}, timeout=10, poll=0) == "COM7"


def test_wait_for_new_port_times_out():
    with patch("flasher.ports.list_port_names", return_value=["COM3"]):
        with patch("flasher.ports.time.sleep"):
            assert wait_for_new_port({"COM3"}, timeout=0.01, poll=0) is None


def test_driver_hint_mentions_ch340():
    assert "CH340" in DRIVER_HINT
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd /home/lucas/split-flap/flashing && python -m pytest flasher/tests/test_ports.py -v`
Expected: FAIL — ImportError.

- [ ] **Step 3: Implement**

```python
# flashing/flasher/ports.py
"""Serial port enumeration + plug-in watching (pyserial)."""
import time

from serial.tools import list_ports

DRIVER_HINT = """\
No serial ports found. On a fresh Windows machine this almost always means
a missing USB-serial driver:
  - Clone Nanos / most adapters: CH340 driver
    -> https://www.wch-ic.com/downloads/CH341SER_ZIP.html
  - NodeMCU-style adapters: CP210x driver
    -> https://www.silabs.com/interface/usb-bridges (CP210x VCP)
Install, replug the USB cable, then retry. The device must show up in
Device Manager under "Ports (COM & LPT)".
"""


def list_port_names() -> list:
    return [p.device for p in list_ports.comports()]


def describe_ports() -> list:
    return [f"{p.device} — {p.description}" for p in list_ports.comports()]


def diff_new(before: set, now: set) -> set:
    return now - before


def wait_for_new_port(before: set, timeout: float = 60.0, poll: float = 1.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        fresh = diff_new(before, set(list_port_names()))
        if fresh:
            return sorted(fresh)[0]
        time.sleep(poll)
    return None
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd /home/lucas/split-flap/flashing && pip install pyserial esptool 2>/dev/null; python -m pytest flasher/tests/test_ports.py -v`
Expected: all passed (pyserial now a dev dependency).

- [ ] **Step 5: Commit**

```bash
git add flashing/flasher/ports.py flashing/flasher/tests/test_ports.py
git commit -m "feat(flashing): serial port enumeration + plug-in watching (#124)"
```

---

### Task 8: `ota.py` part 2 — multipart upload, /settings client, attempt loop

**Files:**
- Modify: `flashing/flasher/ota.py` (append)
- Test: `flashing/flasher/tests/test_ota_client.py`

**Interfaces:**
- Consumes: verdict constants + classifiers from Task 5.
- Produces: `encode_multipart(field: str, filename: str, data: bytes) -> tuple[bytes, str]` (body, content-type), `fetch_settings(base_url: str, timeout: float = 5.0) -> dict | None`, `fetch_setting(base_url: str, key: str) -> str` (`"?"` on unreachable/missing — same soft-fail contract as the shell), `run_ota(base_url, bin_path, git_rev, max_attempts, quiet_mode, say) -> str` (final verdict; `say` = print-like callable).

- [ ] **Step 1: Write the failing tests**

```python
# flashing/flasher/tests/test_ota_client.py
import json
from unittest.mock import patch
from flasher.ota import encode_multipart, fetch_setting


def test_multipart_is_well_formed():
    body, ctype = encode_multipart("firmware", "fw.bin", b"\x01\x02")
    boundary = ctype.split("boundary=")[1]
    assert ctype.startswith("multipart/form-data; boundary=")
    assert body.startswith(b"--" + boundary.encode())
    assert b'name="firmware"' in body
    assert b'filename="fw.bin"' in body
    assert b"\x01\x02" in body
    assert body.endswith(b"--" + boundary.encode() + b"--\r\n")


def test_fetch_setting_soft_fails_to_question_mark():
    with patch("flasher.ota.fetch_settings", return_value=None):
        assert fetch_setting("http://x", "sketchMd5") == "?"
    with patch("flasher.ota.fetch_settings", return_value={"other": 1}):
        assert fetch_setting("http://x", "sketchMd5") == "?"
    with patch("flasher.ota.fetch_settings", return_value={"sketchMd5": "abc"}):
        assert fetch_setting("http://x", "sketchMd5") == "abc"
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd /home/lucas/split-flap/flashing && python -m pytest flasher/tests/test_ota_client.py -v`
Expected: FAIL — ImportError on new names.

- [ ] **Step 3: Implement** (append to `ota.py`)

```python
# append to flashing/flasher/ota.py
import hashlib
import json
import time
import urllib.error
import urllib.request
import uuid


def encode_multipart(field: str, filename: str, data: bytes) -> tuple:
    boundary = uuid.uuid4().hex
    body = (
        f"--{boundary}\r\n"
        f'Content-Disposition: form-data; name="{field}"; filename="{filename}"\r\n'
        f"Content-Type: application/octet-stream\r\n\r\n"
    ).encode() + data + f"\r\n--{boundary}--\r\n".encode()
    return body, f"multipart/form-data; boundary={boundary}"


def fetch_settings(base_url: str, timeout: float = 5.0):
    try:
        with urllib.request.urlopen(f"{base_url}/settings", timeout=timeout) as r:
            return json.loads(r.read().decode())
    except (urllib.error.URLError, ValueError, OSError):
        return None


def fetch_setting(base_url: str, key: str) -> str:
    settings = fetch_settings(base_url)
    if settings is None or key not in settings or settings[key] is None:
        return "?"
    return str(settings[key])


def _enter_ota_mode(base_url: str, say) -> None:
    say("requesting quiet OTA mode...")
    try:
        req = urllib.request.Request(f"{base_url}/firmware/ota-mode", data=b"", method="POST")
        urllib.request.urlopen(req, timeout=10)
    except (urllib.error.URLError, OSError):
        say("/firmware/ota-mode not accepted (pre-#117 firmware) — normal-mode flash")
        return
    deadline = time.monotonic() + 60
    while time.monotonic() < deadline:
        time.sleep(3)
        if fetch_setting(base_url, "isInOtaMode") == "True":
            say("device is in OTA mode — display quiet")
            return
    say("device did not report OTA mode within 60 s — continuing anyway")


def _one_attempt(base_url: str, data: bytes, uploaded_md5: str, git_rev: str, say) -> str:
    pre_md5 = fetch_setting(base_url, "sketchMd5")
    if pre_md5 == "?":
        say(f"device unreachable at {base_url}")
        return UNREACHABLE
    url = f"{base_url}/firmware/master?md5={uploaded_md5}"
    if git_rev:
        url += f"&v={git_rev}"
    body, ctype = encode_multipart("firmware", "firmware.bin", data)
    req = urllib.request.Request(url, data=body, headers={"Content-Type": ctype}, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=180) as r:
            http_code, resp = r.status, r.read().decode(errors="replace")
    except urllib.error.HTTPError as e:
        http_code, resp = e.code, e.read().decode(errors="replace")
    except (urllib.error.URLError, OSError) as e:
        say(f"upload failed: {e}")
        return UPLOAD_FAILED
    say(f"HTTP {http_code}: {resp.strip()[:200]}")
    verdict = classify_upload(http_code, resp)
    if verdict:
        return verdict
    say("master rebooting — polling /settings (up to 90 s)...")
    deadline = time.monotonic() + 90
    post_md5, last_result = "?", "?"
    while time.monotonic() < deadline:
        time.sleep(3)
        post_md5 = fetch_setting(base_url, "sketchMd5")
        if post_md5 not in ("?", ""):
            last_result = fetch_setting(base_url, "lastFlashResult")
            break
    say(f"sketchMd5 {pre_md5} -> {post_md5}   lastFlashResult: {last_result}")
    return classify_post_flash(uploaded_md5, pre_md5, post_md5, last_result)


def run_ota(base_url: str, bin_path: str, git_rev: str, max_attempts: int, quiet_mode: bool, say) -> str:
    from pathlib import Path
    data = Path(bin_path).read_bytes()
    uploaded_md5 = hashlib.md5(data).hexdigest()
    verdict = INCONSISTENT
    for attempt in range(1, max_attempts + 1):
        say(f"=== OTA attempt {attempt} of {max_attempts} ===")
        if quiet_mode:
            _enter_ota_mode(base_url, say)
        verdict = _one_attempt(base_url, data, uploaded_md5, git_rev, say)
        if verdict == SUCCESS or verdict not in RETRYABLE:
            return verdict
        if attempt < max_attempts:
            say("EBOOT SILENT REVERT — retrying in 5 s...")
            time.sleep(5)
    return verdict
```

- [ ] **Step 4: Run all ota tests to verify they pass**

Run: `cd /home/lucas/split-flap/flashing && python -m pytest flasher/tests/test_ota.py flasher/tests/test_ota_client.py -v`
Expected: all passed.

- [ ] **Step 5: Commit**

```bash
git add flashing/flasher/ota.py flashing/flasher/tests/test_ota_client.py
git commit -m "feat(flashing): OTA multipart upload + attempt loop (#124)"
```

---

### Task 9: `ui.py` + `main.py` — prompts and menu

**Files:**
- Create: `flashing/flasher/ui.py`, `flashing/flasher/main.py`
- Test: `flashing/flasher/tests/test_ui.py`

**Interfaces:**
- `ui.py` produces: `enable_ansi() -> None`, `heading(text)`, `ok(text)`, `warn(text)`, `fail(text)`, `say(text)`, `ask(prompt) -> str`, `ask_int(prompt, lo, hi) -> int` (reprompts until valid), `ask_yn(prompt, default=False) -> bool`, `pause(prompt="Press Enter to continue...")`.
- `main.py` produces: `main() -> None` (menu loop). The menu list is function-local by design — its handlers bind to the lazily-imported `wizard` (Task 10), so it cannot exist at module scope. Menu handlers for options 2–7 call into `wizard`/`ota`/`esp`/`wiring`; option 1 calls `wizard.run_wizard`.

- [ ] **Step 1: Write the failing tests** (test the reprompt logic — the only non-trivial part)

```python
# flashing/flasher/tests/test_ui.py
from unittest.mock import patch
from flasher.ui import ask_int, ask_yn


def test_ask_int_reprompts_until_in_range():
    with patch("builtins.input", side_effect=["0", "banana", "17", "12"]):
        assert ask_int("units?", 1, 16) == 12


def test_ask_yn_defaults():
    with patch("builtins.input", return_value=""):
        assert ask_yn("sure?", default=True) is True
        assert ask_yn("sure?", default=False) is False
    with patch("builtins.input", return_value="y"):
        assert ask_yn("sure?", default=False) is True
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd /home/lucas/split-flap/flashing && python -m pytest flasher/tests/test_ui.py -v`
Expected: FAIL — ImportError.

- [ ] **Step 3: Implement**

```python
# flashing/flasher/ui.py
"""Terminal prompts + colors. Windows: enable VT processing, no deps."""
import os
import sys

_GREEN, _YELLOW, _RED, _BOLD, _RESET = "\033[32m", "\033[33m", "\033[31m", "\033[1m", "\033[0m"
_ansi = False


def enable_ansi() -> None:
    global _ansi
    if os.name == "nt":
        import ctypes
        k32 = ctypes.windll.kernel32
        h = k32.GetStdHandle(-11)
        mode = ctypes.c_uint32()
        if k32.GetConsoleMode(h, ctypes.byref(mode)):
            _ansi = bool(k32.SetConsoleMode(h, mode.value | 0x0004))
    else:
        _ansi = sys.stdout.isatty()


def _c(color: str, text: str) -> str:
    return f"{color}{text}{_RESET}" if _ansi else text


def heading(text: str) -> None:
    print(f"\n{_c(_BOLD, '=== ' + text + ' ===')}")


def ok(text: str) -> None:
    print(_c(_GREEN, "[ok] " + text))


def warn(text: str) -> None:
    print(_c(_YELLOW, "[warn] " + text))


def fail(text: str) -> None:
    print(_c(_RED, "[fail] " + text))


def say(text: str) -> None:
    print(text)


def ask(prompt: str) -> str:
    return input(f"{prompt} ").strip()


def ask_int(prompt: str, lo: int, hi: int) -> int:
    while True:
        raw = ask(f"{prompt} ({lo}-{hi}):")
        try:
            v = int(raw)
            if lo <= v <= hi:
                return v
        except ValueError:
            pass
        print(f"  please enter a number between {lo} and {hi}")


def ask_yn(prompt: str, default: bool = False) -> bool:
    suffix = "[Y/n]" if default else "[y/N]"
    raw = ask(f"{prompt} {suffix}").lower()
    if raw == "":
        return default
    return raw in ("y", "yes")


def pause(prompt: str = "Press Enter to continue...") -> None:
    input(prompt)
```

```python
# flashing/flasher/main.py
"""Entry point: banner + menu loop."""
import sys

from flasher import ui, wiring
from flasher.assets import ManifestError, load_manifest


def _wiring_help(manifest):
    names = list(wiring.DIAGRAMS)
    ui.heading("Wiring help")
    for i, name in enumerate(names, 1):
        print(f" {i}. {name}")
    choice = ui.ask_int("Which diagram?", 1, len(names))
    print(wiring.DIAGRAMS[names[choice - 1]])
    ui.pause()


def main() -> None:
    ui.enable_ansi()
    try:
        manifest = load_manifest()
    except ManifestError as e:
        ui.fail(str(e))
        sys.exit(2)

    # Imported here so a broken serial stack still lets wiring help open.
    from flasher import wizard

    menu = [
        ("Provision a new display (guided, start here)", wizard.run_wizard),
        ("Prepare Arduino-as-ISP programmer", wizard.run_programmer_prep),
        ("Flash twiboot to a single unit", wizard.run_single_unit),
        ("Flash master firmware (USB serial)", wizard.run_master_serial),
        ("Update master firmware (WiFi/OTA)", wizard.run_master_ota),
        ("Check display status", wizard.run_status),
        ("Wiring help", _wiring_help),
    ]
    while True:
        ui.heading(f"SPLIT-FLAP FLASHER  (firmware {manifest['git_rev']}, built {manifest['build_date']})")
        for i, (label, _) in enumerate(menu, 1):
            print(f" {i}. {label}")
        print(" 0. Exit")
        choice = ui.ask_int("Choose", 0, len(menu))
        if choice == 0:
            return
        try:
            menu[choice - 1][1](manifest)
        except KeyboardInterrupt:
            ui.warn("interrupted — back to menu (session state saved where applicable)")
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd /home/lucas/split-flap/flashing && python -m pytest flasher/tests/test_ui.py -v`
Expected: all passed.

- [ ] **Step 5: Commit**

```bash
git add flashing/flasher/ui.py flashing/flasher/main.py flashing/flasher/tests/test_ui.py
git commit -m "feat(flashing): terminal UI + menu loop (#124)"
```

---

### Task 10: `wizard.py` — provisioning flow + network verdict

**Files:**
- Create: `flashing/flasher/wizard.py`
- Test: `flashing/flasher/tests/test_wizard.py`

**Interfaces:**
- Consumes: everything above. All menu handlers take `(manifest)`.
- Produces: `network_verdict(settings: dict | None, expected_n: int, manifest_rev: str) -> tuple[bool, list[str]]` (pure), `run_wizard(manifest)`, `run_programmer_prep(manifest)`, `run_single_unit(manifest)`, `run_master_serial(manifest)`, `run_master_ota(manifest)`, `run_status(manifest)`.

- [ ] **Step 1: Write the failing tests** (pure verdict logic — Codex High finding #2 encoded here)

```python
# flashing/flasher/tests/test_wizard.py
from flasher.wizard import network_verdict


def settings(rev="abc1234", detected=3, addrs=(1, 2, 3), width=3):
    return {"version": rev, "detectedUnitCount": detected,
            "detectedUnitAddresses": list(addrs), "unitCount": width}


def test_all_good():
    ok, problems = network_verdict(settings(), 3, "abc1234")
    assert ok and problems == []


def test_uses_detected_count_not_display_width():
    # units 1 and 12 alive -> unitCount(displayWidth)=12 but detected=2. Must fail.
    s = settings(detected=2, addrs=(1, 12), width=12)
    ok, problems = network_verdict(s, 12, "abc1234")
    assert not ok
    assert any("missing" in p for p in problems)


def test_missing_addresses_are_named():
    s = settings(detected=2, addrs=(1, 3), width=3)
    ok, problems = network_verdict(s, 3, "abc1234")
    assert not ok
    assert any("2" in p for p in problems)


def test_rev_mismatch_reported():
    ok, problems = network_verdict(settings(rev="dead999"), 3, "abc1234")
    assert not ok
    assert any("abc1234" in p and "dead999" in p for p in problems)


def test_unreachable():
    ok, problems = network_verdict(None, 3, "abc1234")
    assert not ok and any("unreachable" in p for p in problems)
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd /home/lucas/split-flap/flashing && python -m pytest flasher/tests/test_wizard.py -v`
Expected: FAIL — ImportError.

- [ ] **Step 3: Implement**

```python
# flashing/flasher/wizard.py
"""The guided flows behind every menu entry."""
import time

from flasher import avr, esp, ota, ports, ui, wiring
from flasher.assets import asset_path
from flasher.session import (Session, default_session_path, load_session,
                             next_unit, save_session)


def network_verdict(settings, expected_n: int, manifest_rev: str):
    """Verify a live display against the manifest + requested unit count.

    Uses detectedUnitCount + detectedUnitAddresses — NOT unitCount, which is
    displayWidth (highest responder + 1) and reads N even with dead units.
    """
    if settings is None:
        return False, ["device unreachable — is it on your WiFi? (check router for its IP)"]
    problems = []
    rev = str(settings.get("version", "?"))
    if rev != manifest_rev:
        problems.append(f"running firmware {rev}, this flasher shipped {manifest_rev}")
    addrs = set(settings.get("detectedUnitAddresses", []))
    expected = set(range(1, expected_n + 1))
    missing = sorted(expected - addrs)
    if missing:
        problems.append(f"units missing from I2C bus (addresses): {missing}")
    detected = settings.get("detectedUnitCount", 0)
    if detected != expected_n and not missing:
        problems.append(f"detectedUnitCount {detected} != expected {expected_n}")
    return (not problems), problems


def _pick_port(purpose: str) -> str:
    while True:
        found = ports.describe_ports()
        if not found:
            ui.warn(ports.DRIVER_HINT)
            ui.pause("Fix the driver / plug the device, then press Enter to rescan...")
            continue
        ui.say(f"\nSerial ports ({purpose}):")
        for i, d in enumerate(found, 1):
            ui.say(f" {i}. {d}")
        ui.say(f" {len(found) + 1}. rescan")
        choice = ui.ask_int("Which port?", 1, len(found) + 1)
        if choice <= len(found):
            return found[choice - 1].split(" — ")[0]


def _flash_programmer(port: str) -> bool:
    avrdude, conf = avr.find_avrdude()
    hex_path = str(asset_path("ArduinoISP.hex"))
    for baud in (115200, 57600):
        ui.say(f"flashing ArduinoISP at {baud} baud...")
        code, out = avr.run_avrdude(avr.arduinoisp_args(avrdude, conf, port, baud, hex_path))
        if code == 0:
            return True
    ui.fail("could not flash ArduinoISP — output above; check the board is a stock Uno/Nano")
    ui.say(out[-2000:])
    return False


def run_programmer_prep(manifest, session=None) -> str | None:
    ui.heading("Prepare Arduino-as-ISP programmer")
    print(wiring.DIAGRAMS["programmer"])
    if not ui.ask_yn("Flash ArduinoISP onto the spare board now?", default=True):
        return None
    ui.say("Plug the spare Uno/Nano into USB (WITHOUT the 10uF cap fitted).")
    port = _pick_port("programmer board")
    if not _flash_programmer(port):
        return None
    ui.ok("programmer ready — now fit the 10uF cap (RESET->GND) and wire the 6 ICSP lines")
    print(wiring.DIAGRAMS["icsp"])
    return port


def _flash_one_unit(unit_no: int, port: str) -> bool:
    avrdude, conf = avr.find_avrdude()
    ui.heading(f"Unit {unit_no}")
    ui.say(f"  {wiring.dip_visual(unit_no)}")
    ui.say("  Disconnect the stepper, set the DIP switches, clip the 6 ICSP wires.")
    ui.pause()
    code, out = avr.run_avrdude(avr.probe_args(avrdude, conf, port))
    sig = avr.parse_signature(out)
    if sig != avr.EXPECTED_SIGNATURE:
        ui.fail(f"signature {sig or 'unreadable'} (expected {avr.EXPECTED_SIGNATURE}) — "
                "wiring/power problem; 0xffffff usually means MOSI/MISO swapped or stepper attached")
        return False
    code, out = avr.run_avrdude(avr.icsp_flash_args(avrdude, conf, port,
                                                    str(asset_path("twiboot-atmega328p-16mhz.hex"))))
    if code != 0:
        ui.fail("twiboot flash failed:\n" + out[-2000:])
        return False
    code, out = avr.run_avrdude(avr.fuse_write_args(avrdude, conf, port))
    if code != 0:
        ui.fail("fuse write failed:\n" + out[-2000:])
        return False
    code, out = avr.run_avrdude(avr.fuse_read_args(avrdude, conf, port))
    fuses = avr.parse_fuses(out)
    if not fuses or not avr.fuses_ok(*fuses):
        ui.fail(f"fuse verify failed — read {fuses}, expected L=0xff H=0xdc E=0xfd(&0x07)")
        return False
    ui.ok(f"unit {unit_no}: twiboot installed, fuses verified")
    return True


def run_wizard(manifest) -> None:
    ui.heading("Provision a new display")
    spath = default_session_path()
    session = load_session(spath)
    if session and session.unit_count and next_unit(session) is not None:
        if ui.ask_yn(f"Resume previous run ({len(session.done)}/{session.unit_count} units done)?",
                     default=True):
            pass
        else:
            session = None
    if session is None or not session.unit_count:
        session = Session(unit_count=ui.ask_int("How many units does this display have?", 1, 16))
        save_session(session, spath)

    if session.programmer_port is None:
        if ui.ask_yn("Do you already have an Arduino-as-ISP programmer?", default=False):
            print(wiring.DIAGRAMS["icsp"])
            session.programmer_port = _pick_port("programmer")
        else:
            session.programmer_port = run_programmer_prep(manifest)
            if session.programmer_port is None:
                return
        save_session(session, spath)

    while (unit := next_unit(session)) is not None:
        done_n = len(session.done)
        ui.say(f"\n--- progress: {done_n}/{session.unit_count} done, "
               f"{len(session.skipped)} skipped ---")
        if _flash_one_unit(unit, session.programmer_port):
            session.done.append(unit)
            if unit in session.skipped:
                session.skipped.remove(unit)
        else:
            action = ui.ask("(r)etry / (s)kip for now / (a)bort?").lower()
            if action == "s" and unit not in session.skipped:
                session.skipped.append(unit)
            elif action == "a":
                save_session(session, spath)
                ui.warn("aborted — run option 1 again to resume where you left off")
                return
        save_session(session, spath)

    ui.ok(f"all {session.unit_count} units done")
    run_master_serial(manifest)

    ui.heading("Assemble the display")
    print(wiring.DIAGRAMS["assembly"])
    ui.pause("Assemble + power everything, then press Enter...")
    ui.heading("First boot")
    ui.say("Join WiFi network 'split-flap-<chipid>-setup' (no password); a portal opens\n"
           "(or browse to http://192.168.4.1/). Enter your home WiFi credentials.\n"
           "The master reboots onto your WiFi and auto-installs the unit firmware over I2C.")
    if ui.ask_yn("Verify the live display over the network now?", default=True):
        run_status(manifest, expected_n=session.unit_count)


def run_single_unit(manifest) -> None:
    unit = ui.ask_int("Which unit number?", 1, 16)
    print(wiring.DIAGRAMS["icsp"])
    port = _pick_port("programmer")
    _flash_one_unit(unit, port)


def run_master_serial(manifest) -> None:
    ui.heading("Flash master (ESP-01 over USB)")
    print(wiring.DIAGRAMS["esp_uart"])
    before = set(ports.list_port_names())
    ui.say("Jumper GPIO0 to GND, then plug the USB-UART adapter in now...")
    port = ports.wait_for_new_port(before, timeout=60) or _pick_port("USB-UART adapter")
    ui.say(f"using {port}")
    try:
        warnings = esp.flash_master(port, str(asset_path("master-firmware.bin")))
    except esp.ImageError as e:
        ui.fail(str(e))
        return
    except Exception as e:  # esptool failures surface here
        ui.fail(f"esptool failed: {e}\nCommon causes: GPIO0 not grounded at power-up, "
                "wrong port, adapter can't power the ESP")
        return
    for w in warnings:
        ui.warn(w)
    ui.ok("master flashed — unplug, REMOVE the GPIO0 jumper, plug back in")


def run_master_ota(manifest) -> None:
    ui.heading("Update master over WiFi")
    target = ui.ask("Device address (e.g. http://192.168.1.50):").rstrip("/")
    quiet = ui.ask_yn("Use quiet OTA mode (display stays dark during flash)?", default=True)
    verdict = ota.run_ota(target, str(asset_path("master-firmware.bin")),
                          manifest["git_rev"], max_attempts=4, quiet_mode=quiet, say=ui.say)
    if verdict == ota.SUCCESS:
        ui.ok("SUCCESS — new firmware is running")
    elif verdict == ota.REVERTED:
        ui.fail("EBOOT SILENT REVERT after all retries — power-supply margin problem; "
                "add caps / beefier 3.3V (see CLAUDE.md OTA section)")
    else:
        ui.fail(f"verdict: {verdict} — not retryable (config/network problem, retrying can't fix it)")


def run_status(manifest, expected_n: int | None = None) -> None:
    target = ui.ask("Device address (e.g. http://192.168.1.50):").rstrip("/")
    settings = ota.fetch_settings(target)
    if settings is None:
        ui.fail("unreachable")
        return
    for key in ("version", "deviceName", "sketchMd5", "lastFlashResult",
                "unitCount", "detectedUnitCount", "detectedUnitAddresses"):
        ui.say(f"  {key:24}: {settings.get(key)}")
    if expected_n is None:
        expected_n = ui.ask_int("How many units should be present?", 1, 16)
    good, problems = network_verdict(settings, expected_n, manifest["git_rev"])
    if good:
        ui.ok(f"Display alive — {expected_n}/{expected_n} units, firmware {manifest['git_rev']}")
    else:
        for p in problems:
            ui.fail(p)
```

- [ ] **Step 4: Run the full suite**

Run: `cd /home/lucas/split-flap/flashing && python -m pytest flasher/tests/ -v`
Expected: all passed.

- [ ] **Step 5: Smoke-test the menu in dev mode (no hardware)**

Run: `cd /home/lucas/split-flap/flashing && python -c "from flasher import main, wizard, ui; print('imports ok')"`
Expected: `imports ok`. (Full menu run needs staged assets — Task 12.)

- [ ] **Step 6: Commit**

```bash
git add flashing/flasher/wizard.py flashing/flasher/tests/test_wizard.py
git commit -m "feat(flashing): guided provisioning wizard + network verdict (#124)"
```

---

### Task 11: Vendor ArduinoISP.hex (built once, committed)

**Files:**
- Create: `flashing/flasher/assets-src/arduinoisp/platformio.ini`, `flashing/flasher/assets-src/arduinoisp/src/ArduinoISP.ino` (vendored from arduino-examples), `flashing/flasher/assets/ArduinoISP.hex` (build output, committed), `flashing/flasher/assets/LICENSES.md`

**Interfaces:**
- Produces: the committed `ArduinoISP.hex` consumed by `run_programmer_prep` via `asset_path("ArduinoISP.hex")`.

- [ ] **Step 1: Vendor the sketch**

```bash
mkdir -p /home/lucas/split-flap/flashing/flasher/assets-src/arduinoisp/src
curl -fsSL https://raw.githubusercontent.com/arduino/arduino-examples/main/examples/11.ArduinoISP/ArduinoISP/ArduinoISP.ino \
  -o /home/lucas/split-flap/flashing/flasher/assets-src/arduinoisp/src/ArduinoISP.ino
```

```ini
# flashing/flasher/assets-src/arduinoisp/platformio.ini
; Builds the stock ArduinoISP example into a hex the flasher bundles.
; Target = the SPARE board that becomes the programmer (Uno or Nano,
; same m328p@16MHz image either way). Built once; hex is committed.
[env:arduinoisp]
platform = atmelavr
board = uno
framework = arduino
```

- [ ] **Step 2: Build + copy the hex**

```bash
cd /home/lucas/split-flap/flashing/flasher/assets-src/arduinoisp && pio run
cp .pio/build/arduinoisp/firmware.hex ../../assets/ArduinoISP.hex
```

Expected: `SUCCESS`, hex ~30 KB text.

- [ ] **Step 3: Write LICENSES.md**

```markdown
# flashing/flasher/assets/LICENSES.md
## ArduinoISP.hex
Compiled from the stock ArduinoISP example (arduino/arduino-examples,
examples/11.ArduinoISP), BSD-licensed, vendored unmodified at
`../assets-src/arduinoisp/src/ArduinoISP.ino`. Rebuild: `pio run` there.

## avrdude.exe / avrdude.conf (exe builds only)
avrdude is GPL-2.0. Bundled unmodified from the official release recorded in
manifest.json (`avrdude_version`, `avrdude_source_url` — the corresponding
source for that exact binary). https://github.com/avrdudes/avrdude
```

- [ ] **Step 4: Verify the flasher finds it**

Run: `cd /home/lucas/split-flap/flashing && python -c "from flasher.assets import asset_path; p = asset_path('ArduinoISP.hex'); print(p, p.stat().st_size)"`
Expected: path + nonzero size.

- [ ] **Step 5: Add an `.pio` ignore + commit**

```bash
echo ".pio/" > /home/lucas/split-flap/flashing/flasher/assets-src/arduinoisp/.gitignore
git add flashing/flasher/assets-src flashing/flasher/assets/ArduinoISP.hex flashing/flasher/assets/LICENSES.md
git commit -m "feat(flashing): vendor ArduinoISP hex for programmer bootstrap (#124)"
```

---

### Task 12: `make_manifest.py` — stage / collect / consistency gate

**Files:**
- Create: `flashing/flasher/make_manifest.py`
- Test: `flashing/flasher/tests/test_make_manifest.py`

**Interfaces:**
- Produces (pure): `build_manifest(root: Path, git_rev: str, build_date: str, extra: dict | None = None) -> dict` (hashes every file under root except manifest.json), `consistency_gate(unit_hex_built: Path, unit_hex_staged: Path, unit_rev_staged: Path, built_rev: str) -> None` (raises `GateError` on any mismatch — Codex High finding #1).
- CLI: `python flasher/make_manifest.py stage` (copy Unit build hex + write `.rev` into `firmware/v1/ESPMaster/data/`), `... collect [--avrdude-zip PATH]` (copy firmware artifacts into `flasher/assets/`, run gate, write manifest.json).

- [ ] **Step 1: Write the failing tests**

```python
# flashing/flasher/tests/test_make_manifest.py
import hashlib
import pytest
from flasher.make_manifest import GateError, build_manifest, consistency_gate


def test_build_manifest_hashes_all_files(tmp_path):
    (tmp_path / "a.bin").write_bytes(b"aaa")
    (tmp_path / "sub").mkdir()
    (tmp_path / "sub" / "b.hex").write_bytes(b"bbb")
    (tmp_path / "manifest.json").write_text("{}")  # must be excluded
    m = build_manifest(tmp_path, "abc1234", "2026-07-05")
    assert m["git_rev"] == "abc1234"
    assert m["assets"]["a.bin"] == hashlib.sha256(b"aaa").hexdigest()
    assert m["assets"]["sub/b.hex"] == hashlib.sha256(b"bbb").hexdigest()
    assert "manifest.json" not in m["assets"]


def test_gate_passes_when_consistent(tmp_path):
    built = tmp_path / "built.hex"; built.write_bytes(b"HEX")
    staged = tmp_path / "staged.hex"; staged.write_bytes(b"HEX")
    rev = tmp_path / "unit-firmware.rev"; rev.write_text("abc1234\n")
    consistency_gate(built, staged, rev, "abc1234")  # no raise


def test_gate_rejects_stale_staged_hex(tmp_path):
    built = tmp_path / "built.hex"; built.write_bytes(b"NEW")
    staged = tmp_path / "staged.hex"; staged.write_bytes(b"OLD")
    rev = tmp_path / "r"; rev.write_text("abc1234")
    with pytest.raises(GateError, match="differs"):
        consistency_gate(built, staged, rev, "abc1234")


def test_gate_rejects_rev_mismatch(tmp_path):
    built = tmp_path / "built.hex"; built.write_bytes(b"HEX")
    staged = tmp_path / "staged.hex"; staged.write_bytes(b"HEX")
    rev = tmp_path / "r"; rev.write_text("dead999")
    with pytest.raises(GateError, match="rev"):
        consistency_gate(built, staged, rev, "abc1234")
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd /home/lucas/split-flap/flashing && python -m pytest flasher/tests/test_make_manifest.py -v`
Expected: FAIL — ImportError.

- [ ] **Step 3: Implement**

```python
# flashing/flasher/make_manifest.py
"""Build-side staging + manifest generation (CI and dev).

  stage    copy the freshly built Unit hex + rev sidecar into ESPMaster/data/
           (MUST run between 'pio run Unit' and 'pio run ESPMaster' —
           build_assets.py embeds data/unit-firmware.hex, it does NOT pull
           the Unit build automatically)
  collect  copy firmware artifacts into flasher/assets/, verify the staged
           hex still equals the built hex (anti-drift gate), write manifest

Usage: python flasher/make_manifest.py stage|collect [--avrdude-zip PATH]
"""
import hashlib
import json
import shutil
import subprocess
import sys
import zipfile
from datetime import date
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
UNIT_BUILD = REPO / "firmware/v1/Unit/.pio/build/unit/firmware.hex"
ESP_DATA = REPO / "firmware/v1/ESPMaster/data"
ESP_BUILD = REPO / "firmware/v1/ESPMaster/.pio/build/espmaster"
TWIBOOT = REPO / "firmware/v1/UnitBootloader/prebuilt/twiboot-atmega328p-16mhz.hex"
ASSETS = Path(__file__).resolve().parent / "assets"


class GateError(Exception):
    pass


def _sha256(p: Path) -> str:
    return hashlib.sha256(p.read_bytes()).hexdigest()


def git_rev() -> str:
    out = subprocess.run(["git", "describe", "--always", "--dirty"],
                         cwd=REPO, capture_output=True, text=True, check=True)
    return out.stdout.strip()


def build_manifest(root: Path, git_rev: str, build_date: str, extra: dict | None = None) -> dict:
    assets = {}
    for p in sorted(root.rglob("*")):
        if p.is_file() and p.name != "manifest.json":
            assets[p.relative_to(root).as_posix()] = _sha256(p)
    manifest = {"git_rev": git_rev, "build_date": build_date, "assets": assets}
    if extra:
        manifest.update(extra)
    return manifest


def consistency_gate(unit_hex_built: Path, unit_hex_staged: Path,
                     unit_rev_staged: Path, built_rev: str) -> None:
    if _sha256(unit_hex_built) != _sha256(unit_hex_staged):
        raise GateError(
            "staged ESPMaster/data/unit-firmware.hex differs from the built Unit hex — "
            "the master would auto-push STALE unit firmware. Run 'stage' then rebuild ESPMaster."
        )
    staged_rev = unit_rev_staged.read_text().strip()
    if staged_rev != built_rev:
        raise GateError(f"staged unit rev '{staged_rev}' != built rev '{built_rev}'")


def cmd_stage() -> None:
    shutil.copy2(UNIT_BUILD, ESP_DATA / "unit-firmware.hex")
    (ESP_DATA / "unit-firmware.rev").write_text(git_rev() + "\n")
    print(f"staged {UNIT_BUILD} -> {ESP_DATA}/unit-firmware.hex (rev {git_rev()})")


def cmd_collect(avrdude_zip: str | None) -> None:
    rev = git_rev()
    consistency_gate(UNIT_BUILD, ESP_DATA / "unit-firmware.hex",
                     ESP_DATA / "unit-firmware.rev", rev)
    ASSETS.mkdir(exist_ok=True)
    shutil.copy2(ESP_BUILD / "firmware.bin", ASSETS / "master-firmware.bin")
    shutil.copy2(UNIT_BUILD, ASSETS / "unit-firmware.hex")
    shutil.copy2(TWIBOOT, ASSETS / TWIBOOT.name)
    extra = {}
    if avrdude_zip:
        dest = ASSETS / "avrdude"
        dest.mkdir(exist_ok=True)
        with zipfile.ZipFile(avrdude_zip) as z:
            for name in z.namelist():
                base = Path(name).name
                if base in ("avrdude.exe", "avrdude.conf"):
                    (dest / base).write_bytes(z.read(name))
        extra = {"avrdude_version": Path(avrdude_zip).stem,
                 "avrdude_source_url": "https://github.com/avrdudes/avrdude/releases"}
    manifest = build_manifest(ASSETS, rev, date.today().isoformat(), extra)
    (ASSETS / "manifest.json").write_text(json.dumps(manifest, indent=2))
    print(f"manifest written: rev {rev}, {len(manifest['assets'])} assets")


if __name__ == "__main__":
    if len(sys.argv) < 2 or sys.argv[1] not in ("stage", "collect"):
        sys.exit(__doc__)
    if sys.argv[1] == "stage":
        cmd_stage()
    else:
        zip_arg = None
        if "--avrdude-zip" in sys.argv:
            zip_arg = sys.argv[sys.argv.index("--avrdude-zip") + 1]
        cmd_collect(zip_arg)
```

Note: `build_manifest`'s `git_rev`/`build_date` params shadow the module-level `git_rev()` function inside that function body — rename the params if the linter complains, keeping the test call signature `build_manifest(root, rev, date_str)` positional.

- [ ] **Step 4: Run tests + a real dev collect**

Run: `cd /home/lucas/split-flap/flashing && python -m pytest flasher/tests/test_make_manifest.py -v`
Expected: all passed.

Then a real end-to-end dev staging (builds firmware, ~2 min):
```bash
cd /home/lucas/split-flap/firmware/v1/Unit && pio run
cd /home/lucas/split-flap/flashing && python flasher/make_manifest.py stage
cd /home/lucas/split-flap/firmware/v1/ESPMaster && pio run
cd /home/lucas/split-flap/flashing && python flasher/make_manifest.py collect
python -c "from flasher.assets import load_manifest; print(load_manifest()['git_rev'])"
```
Expected: manifest written; final line prints the current rev. NOTE: `stage` modifies `firmware/v1/ESPMaster/data/unit-firmware.hex|.rev` which are **committed files** — inspect `git diff` after; committing the refreshed hex is correct and expected (that's the mechanism that keeps the bundled unit firmware current).

- [ ] **Step 5: Full suite + menu smoke test**

```bash
cd /home/lucas/split-flap/flashing && python -m pytest flasher/tests/ -v
printf '7\n1\n\n0\n' | python -m flasher   # wiring help -> programmer diagram -> exit
```
Expected: tests pass; menu renders with rev banner, diagram prints, clean exit.

- [ ] **Step 6: Commit**

```bash
git add flashing/flasher/make_manifest.py flashing/flasher/tests/test_make_manifest.py firmware/v1/ESPMaster/data/unit-firmware.hex firmware/v1/ESPMaster/data/unit-firmware.rev
git commit -m "feat(flashing): manifest generator + build staging with anti-drift gate (#124)"
```

---

### Task 13: PyInstaller spec + CI workflow

**Files:**
- Create: `flashing/flasher/flasher.spec`, `.github/workflows/flasher.yml`
- Modify: `.github/workflows/build.yml` (add flasher pytest step to the `test` job, after line 77)

**Interfaces:**
- Consumes: `make_manifest.py` CLI, the whole package.
- Produces: `split-flap-flasher.exe` workflow artifact; release-attached on tags.

- [ ] **Step 1: Write the PyInstaller spec**

```python
# flashing/flasher/flasher.spec
# Build (Windows, from flashing/): pyinstaller flasher/flasher.spec
a = Analysis(
    ["flasher/__main__.py"],
    pathex=["."],
    datas=[("flasher/assets", "assets")],
    hiddenimports=["esptool", "serial", "serial.tools.list_ports"],
)
pyz = PYZ(a.pure)
exe = EXE(
    pyz, a.scripts, a.binaries, a.datas,
    name="split-flap-flasher",
    console=True,
    upx=False,
)
```

- [ ] **Step 2: Determine the avrdude pin**

Download the current avrdude Windows release once, record URL + SHA-256 (concrete values go into the workflow — compute now, do not guess):

```bash
cd /tmp && curl -fsSLO https://github.com/avrdudes/avrdude/releases/download/v7.3/avrdude-v7.3-windows-x64.zip && sha256sum avrdude-v7.3-windows-x64.zip
```

Expected: a hash line. Put the URL and that exact hash into `flasher.yml` below (replace `<SHA256_FROM_STEP_2>`). If v7.3's asset name differs, list assets with `gh release view v7.3 -R avrdudes/avrdude` and adjust.

- [ ] **Step 3: Write the workflow**

```yaml
# .github/workflows/flasher.yml
name: Flasher exe

on:
  push:
    branches: [master]
    paths: ['flashing/**', 'firmware/**', '.github/workflows/flasher.yml']
    tags: ['v*']
  workflow_dispatch:

jobs:
  build-exe:
    runs-on: windows-latest
    steps:
      - uses: actions/checkout@v4

      - uses: actions/setup-python@v5
        with:
          python-version: '3.11'

      - name: Install tools
        run: pip install platformio pyinstaller esptool pyserial

      - name: Build Unit firmware
        working-directory: firmware/v1/Unit
        run: pio run

      - name: Stage unit hex into ESPMaster (order is load-bearing)
        working-directory: flashing
        run: python flasher/make_manifest.py stage

      - name: Build ESPMaster firmware
        working-directory: firmware/v1/ESPMaster
        run: pio run

      - name: Fetch pinned avrdude
        shell: bash
        run: |
          curl -fsSLo avrdude.zip https://github.com/avrdudes/avrdude/releases/download/v7.3/avrdude-v7.3-windows-x64.zip
          echo "<SHA256_FROM_STEP_2>  avrdude.zip" | sha256sum -c -

      - name: Collect assets + manifest (consistency gate)
        working-directory: flashing
        run: python flasher/make_manifest.py collect --avrdude-zip ../avrdude.zip

      - name: Run flasher tests
        working-directory: flashing
        run: pip install pytest && python -m pytest flasher/tests/ -v

      - name: Build exe
        working-directory: flashing
        run: pyinstaller flasher/flasher.spec

      - name: Upload artifact
        uses: actions/upload-artifact@v4
        with:
          name: split-flap-flasher
          path: flashing/dist/split-flap-flasher.exe

      - name: Attach to release
        if: startsWith(github.ref, 'refs/tags/')
        env:
          GH_TOKEN: ${{ github.token }}
        run: gh release upload "${GITHUB_REF_NAME}" flashing/dist/split-flap-flasher.exe --clobber
```

- [ ] **Step 4: Add flasher tests to the Linux CI test job**

Append to `.github/workflows/build.yml` `test` job (after the existing "Run Python tests" step at line 72-77):

```yaml
      - name: Install flasher deps
        run: pip install pyserial esptool

      - name: Run flasher tests
        working-directory: flashing
        run: python -m pytest flasher/tests/ -v
```

- [ ] **Step 5: Commit + verify CI**

```bash
git add flashing/flasher/flasher.spec .github/workflows/flasher.yml .github/workflows/build.yml
git commit -m "ci: build split-flap-flasher.exe on windows-latest with consistency gate (#124)"
git push
gh run watch --exit-status || gh run list --limit 3
```
Expected: both workflows green; `split-flap-flasher` artifact downloadable. If the windows job fails on avrdude asset naming or PyInstaller data paths, fix within this task before proceeding.

---

### Task 14: Cleanup — delete stale scripts, rewrite docs

**Files:**
- Delete: `flashing/1-flash-unit-bootloader.bat`, `flashing/2-flash-master.bat`, `flashing/config.bat`, `flashing/package-flasher.sh`, `flashing/prebuilt/` (stale gitignored bins — remove from disk)
- Rewrite: `flashing/README.md`
- Modify: `firmware/v1/UnitBootloader/README.md` (lines 33, 149, 153-158), `CLAUDE.md` (Build/Flash section — mention the flasher), `README.md` (repo root, if it references the .bat flow)

**Interfaces:** none (docs).

- [ ] **Step 1: Delete the stale scripts**

```bash
cd /home/lucas/split-flap
git rm flashing/1-flash-unit-bootloader.bat flashing/2-flash-master.bat flashing/config.bat flashing/package-flasher.sh
rm -rf flashing/prebuilt flashing/.pytest_cache
```

- [ ] **Step 2: Rewrite `flashing/README.md`** with exactly this content:

```markdown
# Split-Flap — flashing

Everything flash-related now lives in **one tool**: `split-flap-flasher.exe`.
Download it from the latest GitHub release (or the `split-flap-flasher`
artifact of the *Flasher exe* workflow), double-click, and follow the menu.
Windows SmartScreen will warn once (unsigned exe) — "More info → Run anyway".

The exe contains the wizard, esptool, avrdude, and **the firmware images it
flashes** (built fresh by CI from the same commit — see `manifest.json`
baked inside). There is nothing else to download and no stale-binaries drift.

## What it does

1. **Provision a new display** — guided cold start: asks how many units
   (1–16), turns a spare Uno/Nano into an Arduino-as-ISP programmer (no
   Arduino IDE needed), flashes twiboot to every Nano with signature +
   fuse verification, flashes the ESP-01 master, then walks assembly,
   WiFi setup, and a live network verification. Interrupted runs resume.
2. **Prepare programmer / single unit / master serial** — the same steps
   standalone, for redoing one piece.
3. **Update master (WiFi/OTA)** — upload with MD5 + verdict polling
   (same semantics as `ota-master.sh`).
4. **Check display status** — `/settings` pretty-print + unit-count verify.
5. **Wiring help** — every connection diagram (ICSP, ESP-01 UART, DIP,
   display assembly), also shown inline at each step.

Prerequisite on the bench machine: **nothing** (the exe is self-contained).
If no COM port appears when you plug something in, install the CH340 driver
(clone Nanos/adapters) — the tool detects this and shows instructions.

## Only one ICSP flash per Nano — twiboot only

The Unit sketch is bundled inside the master firmware (PROGMEM) and pushed
to each Nano over I2C automatically the first time the master sees it in
bootloader mode. Unit firmware updates ride along with master OTA updates.

## DIP switch addresses

`1` = switch up. The unit firmware offsets the DIP value by 1 for I2C
(address 0x00 is reserved), so DIP 0000 → I2C 0x01. Units must be addressed
contiguously from unit 1.

| Unit | DIP  | I2C  | Unit | DIP  | I2C  |
| ---- | ---- | ---- | ---- | ---- | ---- |
| 1    | 0000 | 0x01 | 9    | 1000 | 0x09 |
| 2    | 0001 | 0x02 | 10   | 1001 | 0x0A |
| 3    | 0010 | 0x03 | 11   | 1010 | 0x0B |
| 4    | 0011 | 0x04 | 12   | 1011 | 0x0C |
| 5    | 0100 | 0x05 | 13   | 1100 | 0x0D |
| 6    | 0101 | 0x06 | 14   | 1101 | 0x0E |
| 7    | 0110 | 0x07 | 15   | 1110 | 0x0F |
| 8    | 0111 | 0x08 | 16   | 1111 | 0x10 |

## Developing the flasher

The tool is a plain Python package (`flasher/`). Dev loop on any OS:

    cd flashing
    pip install pyserial esptool pytest
    python -m pytest flasher/tests/ -v      # pure-logic tests, no hardware
    # stage real firmware assets for a live run:
    (cd ../firmware/v1/Unit && pio run)
    python flasher/make_manifest.py stage
    (cd ../firmware/v1/ESPMaster && pio run)
    python flasher/make_manifest.py collect
    python -m flasher

The exe is built by `.github/workflows/flasher.yml` (windows-latest):
Unit build → stage hex+rev into ESPMaster/data → ESPMaster build →
manifest + consistency gate → PyInstaller. The gate fails the build if the
master's embedded unit firmware doesn't match the freshly built Unit hex.

`ota-master.sh` remains for Linux-side OTA from a dev checkout:
`./ota-master.sh <fw.bin> http://host:port`.
```

- [ ] **Step 3: Fix `firmware/v1/UnitBootloader/README.md`**

Replace line 33 (`The sketch-side opcode and EEPROM magic-byte handshake are **not yet implemented**...`) with:

```markdown
The sketch-side handshake and the master-side flash client shipped long ago:
the master auto-installs the bundled unit firmware on boot for any Nano it
sees in bootloader mode, and `/reflash-units` forces a re-push (see issue #10).
```

Replace line 149 (`Once you see 0x29 respond, you're good to proceed to Phase 2...`) with:

```markdown
Once you see `0x29` respond, the Nano is ready — the master handles
everything else (auto-install on its next boot scan).
```

Replace the status list (lines 153-158) with:

```markdown
- [x] Phase 0 — design in the issue body.
- [x] Phase 1 — this directory. Vendored twiboot, patched for 16 MHz Nano, `.hex` builds and is checked in.
- [x] Phase 2 — sketch-side: EEPROM identity + DIP fallback, jump-to-bootloader I2C opcode.
- [x] Phase 3 — master-side: twiboot protocol client, PROGMEM-bundled hex, auto-install on boot + `/reflash-units`.
- [x] Phase 4 — web UI reflash controls (Maintenance tab).
- [x] Phase 5 — EEPROM layout versioned in `SettingsEepromLayout.h` (natively tested).

Provisioning a whole display (this bootloader + unit firmware + master) is
driven by `split-flap-flasher.exe` — see `flashing/README.md`.
```

- [ ] **Step 4: Sync `CLAUDE.md` + root `README.md`**

In `CLAUDE.md` "Build / Flash / Test Workflow" section, after the OTA sentence, add:

```markdown
First-time provisioning of a physical display (programmer bootstrap, twiboot
per Nano, master USB flash, verify) is driven by `split-flap-flasher.exe` —
a PyInstaller one-file exe built by `.github/workflows/flasher.yml` from
`flashing/flasher/` (plain Python package, pytest-covered; dev: `python -m
flasher` from `flashing/` after `make_manifest.py stage`/`collect`; the
`stage` step MUST run between the Unit and ESPMaster builds).
```

Check root `README.md` for references to the `.bat` flow / `flashing/prebuilt` and update to point at the exe + `flashing/README.md`.

- [ ] **Step 5: Full test suite one last time**

```bash
cd /home/lucas/split-flap/flashing && python -m pytest flasher/tests/ -v
cd /home/lucas/split-flap/firmware/v1/ESPMaster && pio test -e native && python -m pytest tests/
```
Expected: everything green.

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "chore(flashing): retire .bat flow, rewrite docs around split-flap-flasher.exe (#124)"
git push
```

---

## Final gate (after all tasks)

1. Dispatch **code-reviewer** (or cpp-reviewer is N/A here — Python) on the full diff `git diff <first-task-commit>^..HEAD`.
2. **Codex cross-model review** of the same diff (read-only), per the final-gate convention.
3. Fix Critical/High findings, re-run the suite.
4. Download the CI exe artifact → user takes it to the Windows bench for hardware acceptance (the only part that cannot be verified here).
5. Close #124 only after bench acceptance; leave a comment with the artifact link.

## Self-Review Notes

- Spec coverage: menu (T9), wizard incl. assembly stop + verify (T10), wiring/DIP (T2), programmer bootstrap (T10/T11), signature/fuse verify (T3), image gate (T4), OTA semantics (T5/T8), session resume (T6), driver hints (T7), manifest + gate (T1/T12), CI order + avrdude pin + release attach (T13), README/UnitBootloader/CLAUDE.md cleanup + .bat deletion (T14). GPL note (T11 LICENSES.md + manifest extra fields in T12).
- Type consistency: all cross-task calls checked — `asset_path` (T1→T10/T11), verdicts (T5→T8→T10), `Session`/`next_unit` (T6→T10), `find_avrdude` tuple (T3→T10), `flash_master -> list[str]` warnings (T4→T10), `run_ota(say=ui.say)` (T8→T10), manifest keys `git_rev`/`build_date` (T1→T9/T12/T13).
- Known deliberate simplifications: no `--help` CLI args (menu-only, per UX decision); `run_status` asks the address rather than mDNS discovery (YAGNI — router lookup documented); ArduinoISP baud fallback covers old-bootloader Nano programmers.
```
