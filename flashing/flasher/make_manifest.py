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


def build_manifest(root: Path, rev: str, build_date: str, extra: dict | None = None) -> dict:
    assets = {}
    for p in sorted(root.rglob("*")):
        if p.is_file() and p.name != "manifest.json":
            assets[p.relative_to(root).as_posix()] = _sha256(p)
    manifest = {"git_rev": rev, "build_date": build_date, "assets": assets}
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
