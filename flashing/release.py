#!/usr/bin/env python3
"""Cuts a release: publishes the staged images and a signed manifest on the
release site, then tags and creates the GitHub release (#583; design in
docs/superpowers/specs/2026-10-10-release-update-design.md).

    flashing/release.py --notes notes.md [--title "BREAKING: ..."]
    flashing/release.py --channel test

The images are the ones staged in ~/bench-bins for the commit being released:
what a wall downloads is byte for byte what ran on the bench. `--channel test`
publishes under /test/ with no tag and no GitHub release.
"""
from __future__ import annotations

import argparse
import base64
import datetime
import hashlib
import io
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
KEEP_RELEASES = 3
MANIFEST_MAX_BYTES = 2048
RESCUE_OFFSET = 0x830000
# DER SubjectPublicKeyInfo of a P-256 key: this prefix, then the 65-byte point.
P256_SPKI_PREFIX = bytes.fromhex("3059301306072a8648ce3d020106082a8648ce3d030107034200")
CHANNEL_DIR = {"stable": "releases", "test": "test"}
# The flasher page's installer, served from the site itself: this version of
# the npm package, and the registry's integrity value for its tarball.
ESP_WEB_TOOLS_VERSION = "10.4.0"
ESP_WEB_TOOLS_INTEGRITY = ("sha512-3pwkeFFm5Fj7UQo8SJNYK5RXrtNCpq6X9QoI6bMT4GBZWgrJqjn0YvM9ihG74BtM"
                           "oSFYXfmDtkehuxe50PTMPQ==")
ESP_WEB_TOOLS_DIR = "esp-web-tools"


class Refused(Exception):
    """A condition of a release is not met; nothing was published."""


def run(*cmd: str, cwd: Path | None = None, capture: bool = True) -> str:
    done = subprocess.run(cmd, cwd=cwd, text=True, capture_output=capture)
    if done.returncode != 0:
        raise Refused(f"{' '.join(cmd[:3])}… failed: {(done.stderr or '').strip()[-300:]}")
    return (done.stdout or "").strip()


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


# --- the key -----------------------------------------------------------------

def public_key_der_from_header(header: Path) -> bytes:
    """The bytes of RELEASE_PUBLIC_KEY_DER in ReleaseSource.h."""
    match = re.search(r"RELEASE_PUBLIC_KEY_DER\[\]\s*=\s*\{([^}]*)\}", header.read_text())
    if not match:
        raise Refused(f"{header.name} carries no RELEASE_PUBLIC_KEY_DER")
    der = bytes(int(b, 16) for b in re.findall(r"0x([0-9a-fA-F]{2})", match.group(1)))
    if len(der) != len(P256_SPKI_PREFIX) + 65 or not der.startswith(P256_SPKI_PREFIX):
        raise Refused(f"the key in {header.name} is not a P-256 public key")
    return der


def release_host_from_header(header: Path) -> str:
    match = re.search(r'#define\s+RELEASE_HOST\s+"([^"]+)"', header.read_text())
    if not match:
        raise Refused(f"{header.name} names no RELEASE_HOST")
    return match.group(1)


def sign(data: bytes, key: Path, passphrase_file: Path | None) -> bytes:
    """ECDSA P-256 / SHA-256 over `data`; the signature in DER. Without a
    passphrase file openssl asks on the terminal."""
    with tempfile.TemporaryDirectory() as tmp:
        message = Path(tmp) / "message"
        message.write_bytes(data)
        cmd = ["openssl", "dgst", "-sha256", "-sign", str(key)]
        if passphrase_file is not None:
            cmd += ["-passin", f"file:{passphrase_file}"]
        done = subprocess.run(cmd + [str(message)], capture_output=True)
    if done.returncode != 0 or not done.stdout:
        raise Refused("signing failed (wrong passphrase, or not a signing key)")
    return done.stdout


def verifies(data: bytes, signature: bytes, public_der: bytes) -> bool:
    with tempfile.TemporaryDirectory() as tmp:
        tmp_dir = Path(tmp)
        (tmp_dir / "message").write_bytes(data)
        (tmp_dir / "sig").write_bytes(signature)
        (tmp_dir / "pub.der").write_bytes(public_der)
        done = subprocess.run(
            ["openssl", "dgst", "-sha256", "-verify", "pub.der", "-keyform", "DER",
             "-signature", "sig", "message"], cwd=tmp_dir, capture_output=True)
    return done.returncode == 0


# --- the images --------------------------------------------------------------

class Images:
    """The staged files of one release, by the names a build gives them."""

    def __init__(self, bins: Path, rev: str, rescue_rev: str):
        self.rev = rev
        self.rescue_rev = rescue_rev
        self.master = bins / f"firmware-{rev}-master.bin"
        self.row_packed = bins / f"follower-{rev}-gz.bin"
        self.row_plain = bins / f"follower-{rev}.bin"
        self.rescue = bins / f"rescue-{rescue_rev}.bin"
        missing = [p.name for p in (self.master, self.row_packed, self.row_plain, self.rescue)
                   if not p.is_file()]
        if missing:
            raise Refused(f"not staged in {bins}: {', '.join(missing)}")


def newest_rescue_rev(bins: Path, repo: Path, rev: str) -> str:
    """The staged rescue image built at the latest commit that `rev` contains."""
    best: tuple[int, str] | None = None
    for path in bins.glob("rescue-*.bin"):
        candidate = path.name[len("rescue-"):-len(".bin")]
        if not re.fullmatch(r"[0-9a-f]{7,40}", candidate):
            continue
        contained = subprocess.run(["git", "merge-base", "--is-ancestor", candidate, rev],
                                   cwd=repo, capture_output=True).returncode == 0
        if contained:
            when = int(run("git", "log", "-1", "--format=%ct", candidate, cwd=repo))
            if best is None or when > best[0]:
                best = (when, candidate)
    if best is None:
        raise Refused(f"no rescue image in {bins} was built from a commit {rev} contains")
    return best[1]


def merge_factory(images: Images, master_build: Path, out: Path, esptool: list[str]) -> None:
    """Whole-flash image of a master: the build's bootloader + partition table +
    application, and the rescue image in the factory slot."""
    base = master_build / "firmware.factory.bin"
    app = master_build / "firmware.bin"
    if not base.is_file() or not app.is_file() or sha256(app) != sha256(images.master):
        raise Refused(
            f"{master_build} does not hold the build of {images.rev}: build the Master "
            "at that commit (the whole-flash image is merged from its bootloader and "
            "partition table)")
    run(*esptool, "--chip", "esp32s3", "merge-bin", "-o", str(out),
        "0x0", str(base), hex(RESCUE_OFFSET), str(images.rescue))


# --- what is published -------------------------------------------------------

def entry(path: Path, rev: str, tag: str, with_md5: bool = False) -> dict:
    data = path.read_bytes()
    item = {"rev": rev, "path": f"{tag}/{path.name}", "size": len(data),
            "sha256": hashlib.sha256(data).hexdigest()}
    if with_md5:
        item["md5"] = hashlib.md5(data).hexdigest()
    return item


# A board keeps no longer list (RELEASE_UNIT_REVS_MAX in ReleaseManifest.h).
UNIT_REVS_MAX_CHARS = 127


def unit_revs(repo: Path, commit: str) -> list[str]:
    """The unit firmware revs that read current on the release's master: the
    bundle the commit carries and the revs recorded as the same image. Empty
    when the commit carries none, or more than a board keeps: the page then
    says nothing about the units."""
    data = "firmware/v2/Master/data/unit-firmware"
    revs: list[str] = []
    for name in (f"{data}.rev", f"{data}.equiv"):
        try:
            text = run("git", "show", f"{commit}:{name}", cwd=repo)
        except Refused:
            continue
        for line in text.splitlines():
            rev = line.strip()
            if rev and not rev.startswith("#") and rev not in revs:
                revs.append(rev)
    return revs if len(",".join(revs)) <= UNIT_REVS_MAX_CHARS else []


def build_manifest(images: Images, channel: str, tag: str, notes_url: str,
                   commit_time: int, units: list[str] | None = None) -> bytes:
    """`channel` is inside what is signed: a board refuses a manifest of
    another channel, so a trial release cannot be served as a release."""
    manifest = {
        "format": 1,
        "channel": channel,
        "tag": tag,
        "notes": notes_url,
        "commitTime": commit_time,
        "master": entry(images.master, images.rev, tag),
        "row": entry(images.row_packed, images.rev, tag, with_md5=True),
        "rescue": entry(images.rescue, images.rescue_rev, tag),
    }
    if units:
        manifest["units"] = {"revs": units}
    data = (json.dumps(manifest, indent=1) + "\n").encode()
    if len(data) > MANIFEST_MAX_BYTES:
        raise Refused(f"the manifest is {len(data)} bytes; a board takes {MANIFEST_MAX_BYTES}")
    return data


def flasher_manifest(images: Images, tag: str, factory_name: str) -> bytes:
    """What ESP Web Tools reads: one build per chip, each one file at offset 0."""
    return (json.dumps({
        "name": "Split-Flap",
        "version": tag,
        "new_install_prompt_erase": True,
        "builds": [
            {"chipFamily": "ESP32-S3", "parts": [{"path": factory_name, "offset": 0}]},
            {"chipFamily": "ESP8266", "parts": [{"path": images.row_plain.name, "offset": 0}]},
        ],
    }, indent=1) + "\n").encode()


def fetch_esp_web_tools() -> bytes:
    url = (f"https://registry.npmjs.org/esp-web-tools/-/"
           f"esp-web-tools-{ESP_WEB_TOOLS_VERSION}.tgz")
    try:
        with urllib.request.urlopen(url, timeout=60) as answer:
            return answer.read()
    except OSError as why:
        raise Refused(f"ESP Web Tools {ESP_WEB_TOOLS_VERSION} could not be fetched: {why}")


def write_esp_web_tools(site: Path) -> None:
    """Puts the pinned ESP Web Tools build under the site, once per version.
    Nothing of a package whose hash is not the pinned one is unpacked."""
    target = site / ESP_WEB_TOOLS_DIR
    marker = target / "VERSION"
    if marker.is_file() and marker.read_text().strip() == ESP_WEB_TOOLS_VERSION:
        return
    package = fetch_esp_web_tools()
    digest = "sha512-" + base64.b64encode(hashlib.sha512(package).digest()).decode()
    if digest != ESP_WEB_TOOLS_INTEGRITY:
        raise Refused("the ESP Web Tools package is not the pinned one")
    if target.exists():
        shutil.rmtree(target)
    target.mkdir(parents=True)
    prefix = "package/dist/web/"
    with tarfile.open(fileobj=io.BytesIO(package), mode="r:gz") as archive:
        for member in archive.getmembers():
            name = member.name[len(prefix):]
            # The build is one flat directory of scripts.
            if (not member.isfile() or not member.name.startswith(prefix)
                    or not re.fullmatch(r"[A-Za-z0-9._-]+\.js", name)):
                continue
            (target / name).write_bytes(archive.extractfile(member).read())
    if not (target / "install-button.js").is_file():
        raise Refused("the ESP Web Tools package holds no install-button.js")
    marker.write_text(ESP_WEB_TOOLS_VERSION + "\n")


def write_page(site: Path, channel: str, page: Path) -> None:
    """The flasher page: at / for a release, beside latest.json for a trial
    release, which never changes what a visitor of the site is given."""
    target = site if channel == "stable" else site / CHANNEL_DIR[channel]
    target.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(page, target / "index.html")


def prune(channel_dir: Path, keep: int = KEEP_RELEASES) -> list[str]:
    """Removes all but the newest `keep` release directories; newest = written
    last, by the commit time each one's manifest copy records."""
    def when(directory: Path) -> int:
        try:
            return int(json.loads((directory / "manifest.json").read_text())["commitTime"])
        except (OSError, ValueError, KeyError):
            return 0
    releases = sorted((d for d in channel_dir.iterdir() if d.is_dir()), key=when)
    gone = releases[:-keep] if len(releases) > keep else []
    for directory in gone:
        shutil.rmtree(directory)
    return [d.name for d in gone]


def write_release(site: Path, channel: str, tag: str, images: Images, factory: Path,
                  manifest: bytes, signature: bytes) -> Path:
    """Lays one release out under the site; latest.json and its signature are
    written last, so a half-written site never points at missing files."""
    channel_dir = site / CHANNEL_DIR[channel]
    release_dir = channel_dir / tag
    if release_dir.exists():
        raise Refused(f"{CHANNEL_DIR[channel]}/{tag} is already published")
    release_dir.mkdir(parents=True)
    for source in (images.master, images.row_packed, images.row_plain, images.rescue):
        shutil.copyfile(source, release_dir / source.name)
    factory_name = f"factory-{images.rev}-master.bin"
    shutil.copyfile(factory, release_dir / factory_name)
    (release_dir / "flasher.json").write_bytes(flasher_manifest(images, tag, factory_name))
    (release_dir / "manifest.json").write_bytes(manifest)
    prune(channel_dir)
    (channel_dir / "latest.json").write_bytes(manifest)
    (channel_dir / "latest.json.sig").write_text(base64.b64encode(signature).decode() + "\n")
    return release_dir


# --- the conditions of a release ---------------------------------------------

def check_source(repo: Path, rev: str, stable: bool) -> str:
    """The full commit id of `rev`, once the tree and the branch allow a release."""
    if run("git", "status", "--porcelain", cwd=repo):
        raise Refused("the working tree has changes")
    commit = run("git", "rev-parse", f"{rev}^{{commit}}", cwd=repo)
    if stable:
        run("git", "fetch", "--quiet", "origin", "master", cwd=repo)
        contained = subprocess.run(["git", "merge-base", "--is-ancestor", commit, "origin/master"],
                                   cwd=repo, capture_output=True).returncode == 0
        if not contained:
            raise Refused(f"{rev} is not on the pushed master")
    return commit


def check_ci(repo: Path, commit: str) -> None:
    runs = json.loads(run("gh", "run", "list", "--commit", commit, "--json",
                          "status,conclusion", cwd=repo) or "[]")
    if not runs:
        raise Refused(f"no CI run for {commit[:7]}")
    bad = [r for r in runs if r["status"] != "completed" or r["conclusion"] != "success"]
    if bad:
        raise Refused(f"CI is not green on {commit[:7]}")


def check_tag_free(repo: Path, tag: str) -> None:
    if run("git", "tag", "--list", tag, cwd=repo):
        raise Refused(f"{tag} exists: one release a day")


def default_esptool() -> list[str]:
    """An esptool that runs and is recent enough for the S3 and `merge-bin`
    (v4.7). PlatformIO's own is v3; one on PATH may be a broken install."""
    candidates = [["esptool"], ["uv", "run", "--no-project", "--with", "esptool>=4.7",
                                "python", "-m", "esptool"]]
    if os.environ.get("ESPTOOL"):
        candidates.insert(0, os.environ["ESPTOOL"].split())
    for candidate in candidates:
        try:
            done = subprocess.run(candidate + ["version"], capture_output=True, text=True)
        except OSError:
            continue
        version = re.search(r"(\d+)\.(\d+)", done.stdout.splitlines()[-1] if done.stdout else "")
        if done.returncode == 0 and version and (int(version[1]), int(version[2])) >= (4, 7):
            return candidate
    raise Refused("no esptool of v4.7 or later (install it, or set ESPTOOL)")


# --- the run -----------------------------------------------------------------

def parse(argv: list[str]) -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--channel", choices=sorted(CHANNEL_DIR), default="stable")
    p.add_argument("--rev", default="HEAD", help="the commit the staged images were built from")
    p.add_argument("--rescue-rev", help="default: the newest staged rescue image that commit contains")
    p.add_argument("--notes", type=Path, help="release notes (stable only, required)")
    p.add_argument("--title", default="", help='added to the tag in the title, e.g. "BREAKING: …"')
    p.add_argument("--tag", help="default: today's vYYYY.MM.DD (test: test-<rev>)")
    p.add_argument("--bins", type=Path, default=Path.home() / "bench-bins")
    p.add_argument("--key-dir", type=Path,
                   default=Path(os.environ.get("SPLITFLAP_KEY_DIR", Path.home() / ".config/split-flap")))
    p.add_argument("--repo", type=Path, default=REPO)
    p.add_argument("--site-remote", help="default: this repository's origin")
    p.add_argument("--dry-run", action="store_true", help="build and sign, publish nothing")
    p.add_argument("--skip-ci-check", action="store_true", help=argparse.SUPPRESS)
    return p.parse_args(argv)


def release(args: argparse.Namespace) -> str:
    repo: Path = args.repo
    stable = args.channel == "stable"
    header = repo / "firmware/v2/release/ReleaseSource.h"
    public_der = public_key_der_from_header(header)
    host = release_host_from_header(header)

    commit = check_source(repo, args.rev, stable)
    rev = run("git", "rev-parse", "--short=7", commit, cwd=repo)
    tag = args.tag or (datetime.date.today().strftime("v%Y.%m.%d") if stable else f"test-{rev}")
    if stable:
        if args.notes is None or not args.notes.is_file():
            raise Refused("a release needs --notes")
        check_tag_free(repo, tag)
        if not args.skip_ci_check:
            check_ci(repo, commit)

    rescue_rev = args.rescue_rev or newest_rescue_rev(args.bins, repo, commit)
    images = Images(args.bins, rev, rescue_rev)
    commit_time = int(run("git", "log", "-1", "--format=%ct", commit, cwd=repo))
    origin = run("git", "remote", "get-url", "origin", cwd=repo)
    slug = re.sub(r"^.*github\.com[:/]|\.git$", "", origin)
    notes_url = (f"https://github.com/{slug}/releases/tag/{tag}" if stable
                 else f"https://github.com/{slug}/commit/{commit}")

    manifest = build_manifest(images, args.channel, tag, notes_url, commit_time,
                              unit_revs(repo, commit))
    key = args.key_dir / "release-key.pem"
    passphrase = args.key_dir / "release-key.pass"
    signature = sign(manifest, key, passphrase if passphrase.is_file() else None)
    if not verifies(manifest, signature, public_der):
        raise Refused("the key that signed is not the one in ReleaseSource.h: no wall would accept this")

    with tempfile.TemporaryDirectory() as tmp:
        factory = Path(tmp) / "factory.bin"
        merge_factory(images, repo / "firmware/v2/Master/.pio/build/master", factory, default_esptool())
        site = Path(tmp) / "site"
        run("git", "clone", "--quiet", "--branch", "gh-pages", "--single-branch", "--depth", "1",
            args.site_remote or origin, str(site))
        write_esp_web_tools(site)
        write_page(site, args.channel, repo / "flashing/site/index.html")
        release_dir = write_release(site, args.channel, tag, images, factory, manifest, signature)
        files = sorted(p.name for p in release_dir.iterdir())
        if args.dry_run:
            return f"dry run: {CHANNEL_DIR[args.channel]}/{tag} would hold {', '.join(files)}"
        run("git", "add", "-A", cwd=site)
        run("git", "-c", f"user.name={run('git', 'config', 'user.name', cwd=repo)}",
            "-c", f"user.email={run('git', 'config', 'user.email', cwd=repo)}",
            "commit", "--quiet", "-m", f"release: {tag} ({args.channel})", cwd=site)
        run("git", "push", "--quiet", "origin", "gh-pages", cwd=site)

        if stable:
            title = f"{tag} — {args.title}" if args.title else tag
            run("git", "tag", "-a", tag, commit, "-m", title, cwd=repo)
            run("git", "push", "--quiet", "origin", tag, cwd=repo)
            assets = [str(release_dir / name) for name in files if name.endswith(".bin")]
            run("gh", "release", "create", tag, "--latest", "--title", title,
                "--notes-file", str(args.notes), *assets, cwd=repo)
    return f"published https://{host}/{CHANNEL_DIR[args.channel]}/latest.json → {tag} ({rev})"


def main() -> int:
    try:
        print(release(parse(sys.argv[1:])))
    except Refused as why:
        print(f"refused: {why}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
