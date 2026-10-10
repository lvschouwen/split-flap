"""release.py against a throwaway repository, site and key.

What a release must never do is publish something a wall will refuse, or
something that is not what was staged. Each test runs the real steps (git,
openssl) in a temporary directory; only esptool's merge is stood in for.
"""
import base64
import hashlib
import io
import json
import re
import subprocess
import sys
import tarfile
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(HERE))
import release  # noqa: E402


def git(cwd: Path, *args: str) -> str:
    done = subprocess.run(["git", "-c", "user.name=t", "-c", "user.email=t@example.org", *args],
                          cwd=cwd, text=True, capture_output=True, check=True)
    return done.stdout.strip()


def header_for(public_pem: Path) -> str:
    der = subprocess.run(["openssl", "pkey", "-pubin", "-in", str(public_pem), "-outform", "DER"],
                         capture_output=True, check=True).stdout
    body = ", ".join(f"0x{b:02x}" for b in der)
    return ('#define RELEASE_HOST "site.example"\n'
            f"static const uint8_t RELEASE_PUBLIC_KEY_DER[] = {{{body}}};\n")


def make_key(directory: Path) -> Path:
    subprocess.run([str(HERE / "make-release-key.sh"), "--no-passphrase"], check=True,
                   env={"SPLITFLAP_KEY_DIR": str(directory), "PATH": "/usr/bin:/bin"},
                   capture_output=True, stdin=subprocess.DEVNULL)
    return directory


class World:
    """A source repository with one commit, an origin that has a gh-pages
    branch, staged images, a master build directory and a key."""

    def __init__(self, tmp: Path):
        self.tmp = tmp
        self.keys = make_key(tmp / "keys")
        self.origin = tmp / "origin.git"
        subprocess.run(["git", "init", "--quiet", "--bare", "-b", "master", str(self.origin)], check=True)
        self.repo = tmp / "repo"
        self.repo.mkdir()
        git(self.repo, "init", "--quiet", "-b", "master")
        git(self.repo, "config", "user.name", "t")
        git(self.repo, "config", "user.email", "t@example.org")
        git(self.repo, "remote", "add", "origin", str(self.origin))
        header = self.repo / "firmware/v2/release/ReleaseSource.h"
        header.parent.mkdir(parents=True)
        header.write_text(header_for(self.keys / "release-key.pub.pem"))
        (self.repo / ".gitignore").write_text(".pio/\n")
        page = self.repo / "flashing/site/index.html"
        page.parent.mkdir(parents=True)
        page.write_text("the flasher page\n")
        git(self.repo, "add", "-A")
        git(self.repo, "commit", "--quiet", "-m", "first")
        git(self.repo, "push", "--quiet", "origin", "master")
        self.rev = git(self.repo, "rev-parse", "--short=7", "HEAD")
        site = tmp / "site-seed"
        site.mkdir()
        git(site, "init", "--quiet", "-b", "gh-pages")
        (site / "index.html").write_text("site\n")
        git(site, "add", "-A")
        git(site, "commit", "--quiet", "-m", "site")
        git(site, "push", "--quiet", str(self.origin), "gh-pages")
        self.bins = tmp / "bins"
        self.bins.mkdir()
        self.stage(self.rev)
        (self.bins / f"rescue-{self.rev}.bin").write_bytes(b"rescue image")

    def stage(self, rev: str, master: bytes = b"master image") -> None:
        (self.bins / f"firmware-{rev}-master.bin").write_bytes(master)
        (self.bins / f"follower-{rev}-gz.bin").write_bytes(b"row packed")
        (self.bins / f"follower-{rev}.bin").write_bytes(b"row plain")
        build = self.repo / "firmware/v2/Master/.pio/build/master"
        build.mkdir(parents=True, exist_ok=True)
        (build / "firmware.bin").write_bytes(master)
        (build / "firmware.factory.bin").write_bytes(b"bootloader+table+" + master)

    def args(self, *extra: str):
        return release.parse(["--channel", "test", "--repo", str(self.repo), "--bins", str(self.bins),
                              "--key-dir", str(self.keys), *extra])

    def published(self) -> Path:
        out = self.tmp / f"out-{len(list(self.tmp.glob('out-*')))}"
        subprocess.run(["git", "clone", "--quiet", "--branch", "gh-pages", str(self.origin), str(out)],
                       check=True)
        return out


def tools_package(files: dict[str, bytes]) -> bytes:
    """A package shaped like the npm tarball of ESP Web Tools."""
    out = io.BytesIO()
    with tarfile.open(fileobj=out, mode="w:gz") as archive:
        for name, content in files.items():
            info = tarfile.TarInfo(name)
            info.size = len(content)
            archive.addfile(info, io.BytesIO(content))
    return out.getvalue()


def pin(monkeypatch, package: bytes) -> list[int]:
    """Makes `package` what the registry serves and what is pinned. The list
    grows by one for every fetch."""
    fetches: list[int] = []

    def fetch() -> bytes:
        fetches.append(1)
        return package
    monkeypatch.setattr(release, "fetch_esp_web_tools", fetch)
    monkeypatch.setattr(release, "ESP_WEB_TOOLS_INTEGRITY",
                        "sha512-" + base64.b64encode(hashlib.sha512(package).digest()).decode())
    return fetches


TOOLS = {
    "package/dist/web/install-button.js": b"button",
    "package/dist/web/install-dialog-im156JnI.js": b"dialog",
    "package/dist/web/../../../escaped.js": b"outside",
    "package/dist/web/deeper/other.js": b"not flat",
    "package/dist/install-button.js": b"another build",
    "package/README.md": b"readme",
}


@pytest.fixture
def world(tmp_path, monkeypatch):
    pin(monkeypatch, tools_package(TOOLS))
    merge = tmp_path / "merge.py"
    merge.write_text(
        "import sys\n"
        "a = sys.argv[1:]\n"
        "out = a[a.index('-o') + 1]\n"
        "parts = a[a.index('-o') + 2:]\n"
        "open(out, 'wb').write(b'|'.join(open(p, 'rb').read() for p in parts[1::2]))\n")
    monkeypatch.setattr(release, "default_esptool", lambda: [sys.executable, str(merge)])
    return World(tmp_path)


def test_test_channel_publishes_what_was_staged_under_a_signature_that_checks_out(world):
    print(release.release(world.args()))
    site = world.published()
    tag = f"test-{world.rev}"
    manifest_bytes = (site / "test/latest.json").read_bytes()
    manifest = json.loads(manifest_bytes)
    assert manifest["tag"] == tag and manifest["format"] == 1
    assert manifest["channel"] == "test"
    assert manifest["commitTime"] == int(git(world.repo, "log", "-1", "--format=%ct"))
    for part, name in (("master", f"firmware-{world.rev}-master.bin"),
                       ("row", f"follower-{world.rev}-gz.bin"),
                       ("rescue", f"rescue-{world.rev}.bin")):
        published = site / "test" / manifest[part]["path"]
        assert published.read_bytes() == (world.bins / name).read_bytes()
        assert manifest[part]["sha256"] == hashlib.sha256(published.read_bytes()).hexdigest()
        assert manifest[part]["size"] == published.stat().st_size
    assert manifest["row"]["md5"] == hashlib.md5(b"row packed").hexdigest()
    signature = base64.b64decode((site / "test/latest.json.sig").read_text())
    public = release.public_key_der_from_header(world.repo / "firmware/v2/release/ReleaseSource.h")
    assert release.verifies(manifest_bytes, signature, public)
    assert not release.verifies(manifest_bytes.replace(b"master", b"mastes", 1), signature, public)
    flasher = json.loads((site / "test" / tag / "flasher.json").read_text())
    for build in flasher["builds"]:
        assert (site / "test" / tag / build["parts"][0]["path"]).is_file()
    factory = (site / "test" / tag / f"factory-{world.rev}-master.bin").read_bytes()
    assert factory == b"bootloader+table+master image|rescue image"
    assert not (site / "releases").exists()
    assert git(world.repo, "tag") == ""


def test_dry_run_publishes_nothing(world):
    assert "dry run" in release.release(world.args("--dry-run"))
    assert not (world.published() / "test").exists()


def test_a_key_that_is_not_the_one_in_the_header_stops_before_anything_is_published(world, tmp_path):
    other = make_key(tmp_path / "other-keys")
    with pytest.raises(release.Refused, match="not the one in ReleaseSource.h"):
        release.release(world.args("--key-dir", str(other)))
    assert not (world.published() / "test").exists()


def test_refuses_a_changed_tree(world):
    (world.repo / "stray.txt").write_text("x")
    with pytest.raises(release.Refused, match="working tree"):
        release.release(world.args())


def test_refuses_when_an_image_is_not_staged(world):
    (world.bins / f"follower-{world.rev}-gz.bin").unlink()
    with pytest.raises(release.Refused, match="not staged"):
        release.release(world.args())


def test_refuses_a_build_directory_of_another_build(world):
    build = world.repo / "firmware/v2/Master/.pio/build/master"
    (build / "firmware.bin").write_bytes(b"some other build")
    with pytest.raises(release.Refused, match="does not hold the build"):
        release.release(world.args())


def test_the_same_release_is_not_published_twice(world):
    release.release(world.args())
    with pytest.raises(release.Refused, match="already published"):
        release.release(world.args())


def test_a_release_needs_notes_and_a_free_tag(world, tmp_path):
    stable = ["--channel", "stable", "--skip-ci-check", "--tag", "v2030.01.01"]
    with pytest.raises(release.Refused, match="--notes"):
        release.release(world.args(*stable))
    notes = tmp_path / "notes.md"
    notes.write_text("notes\n")
    git(world.repo, "tag", "v2030.01.01")
    with pytest.raises(release.Refused, match="one release a day"):
        release.release(world.args(*stable, "--notes", str(notes)))


def test_a_release_must_be_on_the_pushed_master(world, tmp_path):
    (world.repo / "more.txt").write_text("x")
    git(world.repo, "add", "-A")
    git(world.repo, "commit", "--quiet", "-m", "not pushed")
    notes = tmp_path / "notes.md"
    notes.write_text("notes\n")
    with pytest.raises(release.Refused, match="not on the pushed master"):
        release.release(world.args("--channel", "stable", "--skip-ci-check", "--notes", str(notes)))


def test_prune_keeps_the_newest_three(tmp_path):
    for index, name in enumerate(["a", "b", "c", "d", "e"]):
        directory = tmp_path / name
        directory.mkdir()
        (directory / "manifest.json").write_text(json.dumps({"commitTime": 100 + index}))
    (tmp_path / "latest.json").write_text("{}")
    assert sorted(release.prune(tmp_path)) == ["a", "b"]
    assert sorted(p.name for p in tmp_path.iterdir()) == ["c", "d", "e", "latest.json"]


def test_the_manifest_is_refused_past_what_a_board_takes(world):
    images = release.Images(world.bins, world.rev, world.rev)
    with pytest.raises(release.Refused, match="a board takes"):
        release.build_manifest(images, "test", "t", "https://example.org/" + "x" * 2048, 1)


def test_the_manifest_names_the_unit_firmware_that_reads_current_on_the_release(world):
    assert release.unit_revs(world.repo, "HEAD") == []
    data = world.repo / "firmware/v2/Master/data"
    data.mkdir(parents=True)
    (data / "unit-firmware.rev").write_text("0d90815\n")
    (data / "unit-firmware.equiv").write_text("# the same image\n4fc4816\n0d90815\n")
    git(world.repo, "add", "-A")
    git(world.repo, "commit", "--quiet", "-m", "bundle")
    revs = release.unit_revs(world.repo, "HEAD")
    assert revs == ["0d90815", "4fc4816"]
    images = release.Images(world.bins, world.rev, world.rev)
    manifest = json.loads(release.build_manifest(images, "test", "t", "https://example.org/n", 1, revs))
    assert manifest["units"] == {"revs": ["0d90815", "4fc4816"]}
    assert "units" not in json.loads(release.build_manifest(images, "test", "t", "https://example.org/n", 1))
    # More than a board keeps: not said at all, never cut short.
    (data / "unit-firmware.equiv").write_text("\n".join(f"{i:07x}" for i in range(20)) + "\n")
    git(world.repo, "commit", "--quiet", "-am", "many")
    assert release.unit_revs(world.repo, "HEAD") == []


def test_the_header_of_this_repository_carries_a_p256_key_and_a_host():
    header = HERE.parent / "firmware/v2/release/ReleaseSource.h"
    assert len(release.public_key_der_from_header(header)) == 91
    assert "." in release.release_host_from_header(header)


def test_a_header_with_another_kind_of_key_is_refused(tmp_path):
    header = tmp_path / "ReleaseSource.h"
    header.write_text("static const uint8_t RELEASE_PUBLIC_KEY_DER[] = {0x30, 0x2a, 0x30};\n")
    with pytest.raises(release.Refused, match="not a P-256"):
        release.public_key_der_from_header(header)


# --- the flasher page and its installer ---------------------------------------

def test_a_trial_release_brings_its_own_page_and_leaves_the_sites_alone(world):
    release.release(world.args())
    site = world.published()
    assert (site / "test/index.html").read_text() == "the flasher page\n"
    assert (site / "index.html").read_text() == "site\n"


def test_a_release_puts_the_page_at_the_top_of_the_site(world, tmp_path):
    site = tmp_path / "laid-out"
    release.write_page(site, "stable", world.repo / "flashing/site/index.html")
    assert (site / "index.html").read_text() == "the flasher page\n"
    assert not (site / "releases").exists()


def test_the_installer_is_served_from_the_site_and_only_its_scripts(world):
    release.release(world.args())
    tools = world.published() / "esp-web-tools"
    assert sorted(p.name for p in tools.iterdir()) == [
        "VERSION", "install-button.js", "install-dialog-im156JnI.js"]
    assert (tools / "install-button.js").read_bytes() == b"button"
    assert (tools / "VERSION").read_text().strip() == release.ESP_WEB_TOOLS_VERSION
    assert not list(world.tmp.rglob("escaped.js"))


def test_the_installer_is_fetched_once_per_version(world, monkeypatch):
    fetches = pin(monkeypatch, tools_package(TOOLS))
    release.release(world.args())
    assert len(fetches) == 1
    (world.repo / "more.txt").write_text("x")
    git(world.repo, "add", "-A")
    git(world.repo, "commit", "--quiet", "-m", "second")
    world.stage(git(world.repo, "rev-parse", "--short=7", "HEAD"))
    release.release(world.args())
    assert len(fetches) == 1
    monkeypatch.setattr(release, "ESP_WEB_TOOLS_VERSION", "99.0.0")
    with pytest.raises(release.Refused, match="already published"):
        release.release(world.args())
    assert len(fetches) == 2


def test_a_package_that_is_not_the_pinned_one_stops_the_release(world, monkeypatch):
    monkeypatch.setattr(release, "fetch_esp_web_tools",
                        lambda: tools_package({"package/dist/web/install-button.js": b"evil"}))
    with pytest.raises(release.Refused, match="not the pinned one"):
        release.release(world.args())
    site = world.published()
    assert not (site / "esp-web-tools").exists() and not (site / "test").exists()


# --- the page, the script and the boards agree (spec, section 11) -------------

REAL_REPO = HERE.parent
PAGE = REAL_REPO / "flashing/site/index.html"


def test_the_page_reads_what_the_script_publishes():
    page = PAGE.read_text()
    assert '"latest.json"' in page and '"/flasher.json"' in page
    assert f'"{release.ESP_WEB_TOOLS_DIR}/install-button.js"' in page
    assert f'"{release.CHANNEL_DIR["stable"]}/"' in page
    assert f"/{release.CHANNEL_DIR['test']}\\/" in page


def test_the_page_loads_no_script_from_another_site():
    page = PAGE.read_text()
    assert not re.search(r"<script[^>]*\ssrc=", page)
    script = page[page.index("<script>"):]
    assert not re.search(r"[\"']https?://", script)
    assert "import(" not in script


def test_the_boards_look_where_the_script_publishes():
    header = (REAL_REPO / "firmware/v2/release/ReleaseManifest.h").read_text()
    dirs = dict(re.findall(r'strcmp\(channel, "(\w+)"\) == 0\) return "(\w+)";', header))
    assert dirs == release.CHANNEL_DIR
    fetch = (REAL_REPO / "firmware/v2/release/ReleaseFetch.h").read_text()
    assert '"latest.json"' in fetch and '"latest.json.sig"' in fetch


def test_the_pinned_installer_is_one_exact_version():
    assert re.fullmatch(r"\d+\.\d+\.\d+", release.ESP_WEB_TOOLS_VERSION)
    assert re.fullmatch(r"sha512-[A-Za-z0-9+/]{86}==", release.ESP_WEB_TOOLS_INTEGRITY)


def test_nothing_of_the_package_is_written_outside_its_directory(tmp_path, monkeypatch):
    pin(monkeypatch, tools_package(TOOLS))
    site = tmp_path / "a/b/c/site"
    site.mkdir(parents=True)
    release.write_esp_web_tools(site)
    written = sorted(str(p.relative_to(tmp_path)) for p in tmp_path.rglob("*") if p.is_file())
    assert written == [f"a/b/c/site/esp-web-tools/{name}" for name in
                       ("VERSION", "install-button.js", "install-dialog-im156JnI.js")]
