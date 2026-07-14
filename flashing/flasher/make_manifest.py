"""Unit-bundle staging + drift gate (CI and dev).

  stage    copy the freshly built Unit hex + rev sidecar into the v2
           master's data/ tree (#205) so its build embeds the current unit
           firmware. MUST run between 'pio run Unit' and the master build —
           build_assets.py embeds data/unit-firmware.hex as-is, it does NOT
           pull the Unit build automatically.
  gate     verify no commit has touched the Unit sources since the staged
           bundle's rev (anti-drift gate, run by CI): a drifted bundle means
           the master auto-pushes stale unit firmware to every Nano it
           reflashes. The gate compares REVS, not bytes — the Unit binary
           embeds GIT_REV (build_version.py), so a fresh build never
           byte-matches the committed bundle. Needs full git history
           (CI checkout with fetch-depth: 0).

firmware/v1/ESPMaster is frozen (#283): its committed bundle is a fossil of
the last pre-freeze stage and is no longer written or gated here.

Usage: python flasher/make_manifest.py stage|gate
"""
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
UNIT_BUILD = REPO / "firmware/v1/Unit/.pio/build/unit/firmware.hex"
UNIT_REV_BUILT = REPO / "firmware/v1/Unit/.pio/build/unit/firmware.rev"
V2_MASTER_DATA = REPO / "firmware/v2/Master/data"
V2_FOLLOWER_ESP01_DATA = REPO / "firmware/v2/FollowerEsp01/data"
# Every tree that embeds the unit bundle (#205; FollowerEsp01 joined at
# #298). stage writes all of them; gate checks all of them.
STAGE_DATA_DIRS = [V2_MASTER_DATA, V2_FOLLOWER_ESP01_DATA]
# Everything that compiles into the Unit binary; host-side tests don't.
UNIT_SRC_PATHSPECS = [
    "firmware/v1/Unit",
    ":(exclude)firmware/v1/Unit/test",
    "firmware/v1/shared",
]


class GateError(Exception):
    pass


def stage_bundle(unit_hex: Path, unit_rev: Path, data_dirs: list[Path]) -> None:
    for data_dir in data_dirs:
        shutil.copy2(unit_hex, data_dir / "unit-firmware.hex")
        shutil.copy2(unit_rev, data_dir / "unit-firmware.rev")
        print(f"staged {unit_hex} -> {data_dir}/unit-firmware.hex")
        print(f"staged {unit_rev} -> {data_dir}/unit-firmware.rev")


def unit_source_head(repo: Path | None = None) -> str:
    """Full hash of the last commit that touched the Unit firmware sources."""
    return subprocess.run(
        ["git", "log", "-1", "--format=%H", "--", *UNIT_SRC_PATHSPECS],
        cwd=repo or REPO, capture_output=True, text=True, check=True,
    ).stdout.strip()


def staged_rev_gate(staged_rev: str, repo: Path | None = None) -> None:
    """The staged bundle's rev must already contain the latest Unit source
    change, i.e. unit_source_head() is an ancestor of (or equal to) it.
    Caveat: squash-merging a PR that contains a stage commit rewrites the
    hash the .rev points to and makes it unresolvable (fails safe)."""
    repo = repo or REPO
    if not staged_rev:
        raise GateError("staged unit-firmware.rev is empty — run 'stage' "
                        "from a clean Unit build and commit the bundle.")
    if staged_rev.endswith("-dirty"):
        raise GateError(
            f"staged unit rev '{staged_rev}' was built from a dirty tree — "
            "commit the Unit change first, rebuild clean, then 'stage'."
        )
    known = subprocess.run(
        ["git", "rev-parse", "--verify", "--quiet", f"{staged_rev}^{{commit}}"],
        cwd=repo, capture_output=True,
    ).returncode == 0
    if not known:
        raise GateError(f"staged unit rev '{staged_rev}' is not a commit in "
                        "this repo — was the bundle staged from another tree?")
    head = unit_source_head(repo)
    if not head:
        raise GateError("no commit touching the Unit sources is visible — "
                        "shallow clone? The gate needs full history "
                        "(fetch-depth: 0).")
    contained = subprocess.run(
        ["git", "merge-base", "--is-ancestor", head, staged_rev],
        cwd=repo, capture_output=True,
    ).returncode == 0
    if not contained:
        raise GateError(
            f"Unit sources changed ({head[:7]}) after the bundle was staged "
            f"at {staged_rev} — the master would auto-push STALE unit "
            "firmware. Rebuild Unit clean, run 'stage', rebuild the v2 "
            "master, and commit the refreshed bundle."
        )


def cmd_stage() -> None:
    if not UNIT_REV_BUILT.exists():
        sys.exit(
            f"error: {UNIT_REV_BUILT} not found — build the Unit sketch first "
            "('pio run' in firmware/v1/Unit)"
        )
    stage_bundle(UNIT_BUILD, UNIT_REV_BUILT, STAGE_DATA_DIRS)


def cmd_gate() -> None:
    for data_dir in STAGE_DATA_DIRS:
        sidecar = data_dir / "unit-firmware.rev"
        if not sidecar.exists():
            raise GateError(f"{sidecar} is missing — run 'stage' from a "
                            "clean Unit build and commit the bundle.")
        staged_rev = sidecar.read_text().strip()
        staged_rev_gate(staged_rev, repo=REPO)
        print(f"gate passed: {data_dir / 'unit-firmware.rev'} rev "
              f"{staged_rev} contains the latest Unit source change")


if __name__ == "__main__":
    if len(sys.argv) < 2 or sys.argv[1] not in ("stage", "gate"):
        sys.exit(__doc__)
    if sys.argv[1] == "stage":
        cmd_stage()
    else:
        cmd_gate()
