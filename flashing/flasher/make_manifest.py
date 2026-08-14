"""Unit-bundle staging + drift gate (CI and dev).

  stage    copy the freshly built Unit hex + rev sidecar into the v2
           master's data/ tree (#205) so its build embeds the current unit
           firmware. MUST run between 'pio run Unit' and the master build —
           build_assets.py embeds data/unit-firmware.hex as-is, it does NOT
           pull the Unit build automatically.
  gate     verify the staged bundle's rev IS the Unit source head (anti-drift
           gate, run by CI): a drifted bundle means the master auto-pushes
           stale unit firmware to every Nano it reflashes. The gate compares
           REVS, not bytes — the Unit binary embeds GIT_REV
           (build_version.py), so a fresh build never byte-matches the
           committed bundle. Needs full git history (CI checkout with
           fetch-depth: 0).

Unit firmware identity is the UNIT SOURCE HEAD — the last commit touching the
files that compile into the Nano binary — not the repo HEAD at build time
(#440). build_version.py stamps both the binary and the .rev sidecar from it,
so the two sides agree by construction and a commit that cannot change the
unit (docs, a master-only fix, a bundle restage) no longer re-labels all 21
units OUTDATED. The gate enforces that invariant with equality: a sidecar
that is merely a DESCENDANT of the source head would disagree with the rev
baked into the binary, and every unit would read OUTDATED against its own
bundle.

Units flashed before that change still report a repo-HEAD rev. equivalent-revs
.txt records the ones measured to run byte-identical machine code, each paired
with the CONTENT HASH of the image they run (the program bytes with the
embedded rev string masked). stage hashes the image it is staging the same way
and carries forward only the entries that still match. Anchoring on the
artifact rather than on a commit is what makes the file safe to leave in the
tree: an entry survives every commit that cannot change the Nano binary and
evaporates the instant one that can does — no one has to remember to prune it,
and a stale entry cannot outlive its proof.

firmware/v1/ESPMaster is frozen (#283): its committed bundle is a fossil of
the last pre-freeze stage and is no longer written or gated here.

Usage: python flasher/make_manifest.py stage|gate
"""
import hashlib
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
UNIT_BUILD = REPO / "firmware/v2/Unit/.pio/build/unit/firmware.hex"
UNIT_REV_BUILT = REPO / "firmware/v2/Unit/.pio/build/unit/firmware.rev"
V2_MASTER_DATA = REPO / "firmware/v2/Master/data"
V2_FOLLOWER_ESP01_DATA = REPO / "firmware/v2/FollowerEsp01/data"
# Every tree that embeds the unit bundle (#205; FollowerEsp01 joined at
# #298). stage writes all of them; gate checks all of them.
STAGE_DATA_DIRS = [V2_MASTER_DATA, V2_FOLLOWER_ESP01_DATA]
# Everything that compiles into the Unit binary; host-side tests don't.
UNIT_SRC_PATHSPECS = [
    "firmware/v2/Unit",
    ":(exclude)firmware/v2/Unit/test",
    ":(exclude)firmware/v2/Unit/equivalent-revs.txt",
    "firmware/v2/shared",
]
# "<legacy-rev> <content-hash-of-the-image-it-runs>" per line, # comments ok.
EQUIVALENT_REVS_FILE = "firmware/v2/Unit/equivalent-revs.txt"
# Below these a field is too short to identify what it claims to, and a
# truncated or typo'd line would start matching things it should not.
REV_MIN_LEN = 7
HASH_MIN_LEN = 32


class GateError(Exception):
    pass


def stage_bundle(unit_hex: Path, unit_rev: Path, data_dirs: list[Path],
                 equivalents: list[str] | None = None) -> None:
    # The .equiv sidecar is ALWAYS written, empty included: it must describe
    # this stage and nothing else. A leftover from a previous stage would go
    # on suppressing an OUTDATED that has since become real.
    body = "".join(f"{rev}\n" for rev in (equivalents or []))
    for data_dir in data_dirs:
        shutil.copy2(unit_hex, data_dir / "unit-firmware.hex")
        shutil.copy2(unit_rev, data_dir / "unit-firmware.rev")
        (data_dir / "unit-firmware.equiv").write_text(body)
        print(f"staged {unit_hex} -> {data_dir}/unit-firmware.hex")
        print(f"staged {unit_rev} -> {data_dir}/unit-firmware.rev")
        print(f"staged {len(equivalents or [])} equivalent rev(s) -> "
              f"{data_dir}/unit-firmware.equiv")


def ihex_to_image(hex_path: Path) -> bytes:
    """Flatten an Intel HEX file into its program image (0xFF for gaps)."""
    mem: dict[int, int] = {}
    base = 0
    for line in hex_path.read_text().splitlines():
        line = line.strip()
        if not line.startswith(":"):
            continue
        raw = bytes.fromhex(line[1:])
        count, addr, rtype, data = raw[0], (raw[1] << 8) | raw[2], raw[3], raw[4:4 + raw[0]]
        if rtype == 0:
            for i, b in enumerate(data):
                mem[base + addr + i] = b
        elif rtype == 2:
            base = ((data[0] << 8) | data[1]) * 16
        elif rtype == 4:
            base = ((data[0] << 8) | data[1]) << 16
    if not mem:
        return b""
    return bytes(mem.get(a, 0xFF) for a in range(max(mem) + 1))


def image_content_hash(hex_path: Path, rev_tag: str) -> str:
    """sha256 of the program image with the embedded rev string blanked.

    The rev tag is the ONLY thing that differs between builds of identical
    sources — measured 2026-08-14, a build at the deployed rev and one at HEAD
    differed in exactly those 7 bytes out of 13848 — so masking it turns
    "which commit" into "which code", which is the question the OUTDATED flag
    is actually asking (#440)."""
    image = ihex_to_image(hex_path)
    if rev_tag:
        image = image.replace(rev_tag.encode(), b"\x00" * len(rev_tag))
    return hashlib.sha256(image).hexdigest()


def load_equivalent_revs(content_hash: str, repo: Path | None = None) -> list[str]:
    """Revs whose image content-hashes to `content_hash` — i.e. units running
    byte-identical machine code to the bundle, under an older rev.

    Anchoring on the ARTIFACT rather than on a commit is what makes this
    safe to leave in the tree: the entry survives every commit that cannot
    change the Nano binary (a shared header the unit never compiles, a docs
    edit, a restage) and evaporates the instant one that can does, with no
    one having to remember to prune it. Malformed lines are skipped rather
    than fatal: a typo must never widen what counts as current."""
    path = (repo or REPO) / EQUIVALENT_REVS_FILE
    if not path.exists():
        return []
    out = []
    for line in path.read_text().splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        fields = line.split()
        if len(fields) != 2:
            continue
        legacy, proven_hash = fields
        if len(legacy) < REV_MIN_LEN or len(proven_hash) < HASH_MIN_LEN:
            continue
        if content_hash and proven_hash == content_hash:
            out.append(legacy)
    return out


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
    # Contained but not equal: staged from a descendant. Harmless for the
    # bytes, fatal for the LABEL — build_version.py stamps the binary with
    # unit_source_head(), so a sidecar carrying anything else disagrees with
    # every unit built from it and the whole fleet reads OUTDATED (#440).
    staged_full = subprocess.run(
        ["git", "rev-parse", f"{staged_rev}^{{commit}}"],
        cwd=repo, capture_output=True, text=True, check=True,
    ).stdout.strip()
    if staged_full != head:
        raise GateError(
            f"staged unit rev '{staged_rev}' is not the Unit source head "
            f"({head[:7]}) — it contains the change but is not the commit "
            "that made it, so it will not match the rev baked into the unit "
            "binary. Rebuild Unit clean and re-run 'stage'."
        )


def cmd_stage() -> None:
    if not UNIT_REV_BUILT.exists():
        sys.exit(
            f"error: {UNIT_REV_BUILT} not found — build the Unit sketch first "
            "('pio run' in firmware/v2/Unit)"
        )
    staged_rev = UNIT_REV_BUILT.read_text().strip()
    content_hash = image_content_hash(UNIT_BUILD, staged_rev)
    print(f"[stage] unit image content hash (rev masked): {content_hash}")
    stage_bundle(UNIT_BUILD, UNIT_REV_BUILT, STAGE_DATA_DIRS,
                 equivalents=load_equivalent_revs(content_hash))


def cmd_gate() -> None:
    for data_dir in STAGE_DATA_DIRS:
        sidecar = data_dir / "unit-firmware.rev"
        if not sidecar.exists():
            raise GateError(f"{sidecar} is missing — run 'stage' from a "
                            "clean Unit build and commit the bundle.")
        staged_rev = sidecar.read_text().strip()
        staged_rev_gate(staged_rev, repo=REPO)
        print(f"gate passed: {data_dir / 'unit-firmware.rev'} rev "
              f"{staged_rev} IS the Unit source head")


if __name__ == "__main__":
    if len(sys.argv) < 2 or sys.argv[1] not in ("stage", "gate"):
        sys.exit(__doc__)
    if sys.argv[1] == "stage":
        cmd_stage()
    else:
        cmd_gate()
