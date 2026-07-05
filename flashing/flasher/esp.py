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
        try:
            esptool.main(args)
        except SystemExit as exc:
            code = exc.code
            if code not in (None, 0):
                raise RuntimeError(f"esptool exited with {code}") from exc
    else:
        subprocess.run([sys.executable, "-m", "esptool"] + args, check=True)
    return warnings
