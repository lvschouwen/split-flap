"""Build-side staging + manifest generation (CI and dev).

  stage    copy the freshly built Unit hex + rev sidecar into BOTH masters'
           data/ trees — v1 ESPMaster and v2 Master (#205) — so the two
           bundles can never drift. MUST run between 'pio run Unit' and the
           master builds — each build_assets.py embeds its own
           data/unit-firmware.hex, it does NOT pull the Unit build
           automatically.
  collect  copy firmware artifacts into flasher/assets/, verify every staged
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
UNIT_REV_BUILT = REPO / "firmware/v1/Unit/.pio/build/unit/firmware.rev"
ESP_DATA = REPO / "firmware/v1/ESPMaster/data"
ESP_BUILD = REPO / "firmware/v1/ESPMaster/.pio/build/espmaster"
V2_MASTER_DATA = REPO / "firmware/v2/Master/data"
V2_MASTER_BUILD = REPO / "firmware/v2/Master/.pio/build/master"
# Every tree that embeds the unit bundle (#205). stage writes all of them;
# collect gates all of them.
STAGE_DATA_DIRS = [ESP_DATA, V2_MASTER_DATA]
TWIBOOT = REPO / "firmware/v1/UnitBootloader/prebuilt/twiboot-atmega328p-16mhz.hex"
ASSETS = Path(__file__).resolve().parent / "assets"


class GateError(Exception):
    pass


def _sha256(p: Path) -> str:
    return hashlib.sha256(p.read_bytes()).hexdigest()


def git_rev() -> str:
    """Master firmware GIT_REV, mirroring
    firmware/v1/ESPMaster/build_assets.py::git_short_rev exactly:
    short HEAD hash, plus a '-dirty' suffix if the tree has uncommitted
    changes. Do NOT use 'git describe' — it picks up old tags and produces
    a value that never matches the firmware's own GIT_REV.
    """
    rev = subprocess.run(["git", "rev-parse", "--short", "HEAD"],
                        cwd=REPO, capture_output=True, text=True, check=True).stdout.strip()
    dirty = bool(subprocess.run(["git", "status", "--porcelain"],
                              cwd=REPO, capture_output=True, text=True, check=True).stdout.strip())
    return f"{rev}-dirty" if dirty else rev


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
                     unit_rev_built: Path, unit_rev_staged: Path) -> None:
    if _sha256(unit_hex_built) != _sha256(unit_hex_staged):
        raise GateError(
            "staged ESPMaster/data/unit-firmware.hex differs from the built Unit hex — "
            "the master would auto-push STALE unit firmware. Run 'stage' then rebuild ESPMaster."
        )
    built_rev = unit_rev_built.read_text().strip()
    staged_rev = unit_rev_staged.read_text().strip()
    if built_rev != staged_rev:
        raise GateError(
            f"staged unit rev '{staged_rev}' != built unit rev '{built_rev}' — "
            "run 'stage' then rebuild ESPMaster."
        )


def freshness_gate(master_bin: Path, staged_hex: Path) -> None:
    """Guard against shipping a master binary that was built BEFORE the
    currently-staged unit firmware — it would embed a stale unit image even
    though the byte-for-byte consistency_gate() above passes (that gate only
    proves the built Unit hex still matches what's staged, not that the
    master was rebuilt afterwards).

    CI's fixed step order (Unit build -> stage -> ESPMaster build -> collect,
    see flasher.yml) always satisfies this naturally. This guard exists for
    manual/dev runs where a rebuild step can be forgotten (e.g. staging fresh
    unit firmware, then collecting without rebuilding ESPMaster first).
    """
    if master_bin.stat().st_mtime < staged_hex.stat().st_mtime:
        raise GateError(
            f"{master_bin} is OLDER than the staged {staged_hex.name} — "
            "ESPMaster was built before this unit firmware was staged, so it "
            "embeds a stale copy. Rebuild ESPMaster ('pio run' in "
            "firmware/v1/ESPMaster) after 'stage', then re-run collect."
        )


def optional_freshness_gate(master_bin: Path, staged_hex: Path) -> None:
    """freshness_gate() for a master that is not part of this collect's
    shipped artifacts (the v2 master, #205): a dev machine that never built
    it must still collect, but a stale existing build fails loudly."""
    if not master_bin.exists():
        print(f"note: {master_bin} not built — skipping its freshness gate")
        return
    freshness_gate(master_bin, staged_hex)


def stage_bundle(unit_hex: Path, unit_rev: Path, data_dirs: list[Path]) -> None:
    for data_dir in data_dirs:
        shutil.copy2(unit_hex, data_dir / "unit-firmware.hex")
        shutil.copy2(unit_rev, data_dir / "unit-firmware.rev")
        print(f"staged {unit_hex} -> {data_dir}/unit-firmware.hex")
        print(f"staged {unit_rev} -> {data_dir}/unit-firmware.rev")


def cmd_stage() -> None:
    if not UNIT_REV_BUILT.exists():
        sys.exit(
            f"error: {UNIT_REV_BUILT} not found — build the Unit sketch first "
            "('pio run' in firmware/v1/Unit) before staging"
        )
    stage_bundle(UNIT_BUILD, UNIT_REV_BUILT, STAGE_DATA_DIRS)


def cmd_collect(avrdude_zip: str | None) -> None:
    rev = git_rev()
    for data_dir in STAGE_DATA_DIRS:
        consistency_gate(UNIT_BUILD, data_dir / "unit-firmware.hex",
                         UNIT_REV_BUILT, data_dir / "unit-firmware.rev")
    freshness_gate(ESP_BUILD / "firmware.bin", ESP_DATA / "unit-firmware.hex")
    optional_freshness_gate(V2_MASTER_BUILD / "firmware.bin",
                            V2_MASTER_DATA / "unit-firmware.hex")
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
