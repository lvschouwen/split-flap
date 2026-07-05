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
