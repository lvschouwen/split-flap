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
