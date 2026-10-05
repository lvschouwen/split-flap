"""The gzip OTA image against the bootloader that has to unpack it (#540).

An ESP-01 row has no second app slot and no serial: eboot unpacking the
stored upload over the running image is the one step nothing can retry. So
this runs the ESP8266 core's own eboot copy_raw() — compiled for the host
from the installed core — over a simulated flash laid out the way
POST /firmware/master leaves it, and requires the image to come out byte for
byte. It also pins what fwbuild's size arithmetic mirrors: the linker
script's app area and the handler's free-space formula.

Skipped where the ESP8266 core or a host C compiler is missing.
Run from firmware/v2/FollowerEsp01: python -m pytest tests/
"""

import os
import pathlib
import random
import re
import shutil
import subprocess
import sys

import pytest

HERE = pathlib.Path(__file__).resolve().parent
PROJECT = HERE.parent
sys.path.insert(0, str(PROJECT.parent / "buildtools"))

import fwbuild  # noqa: E402

FLASH_SIZE = 0x100000
SECTOR = 0x1000


def _core_dir():
    root = pathlib.Path(os.environ.get(
        "PLATFORMIO_CORE_DIR", pathlib.Path.home() / ".platformio"))
    core = root / "packages" / "framework-arduinoespressif8266"
    return core if (core / "bootloaders" / "eboot" / "eboot.c").exists() else None


CORE = _core_dir()
CC = shutil.which("gcc") or shutil.which("cc")


@pytest.fixture(scope="module")
def harness(tmp_path_factory):
    if CORE is None or CC is None:
        pytest.skip("needs the installed ESP8266 core and a host C compiler")
    build = tmp_path_factory.mktemp("eboot")
    src = (CORE / "bootloaders" / "eboot" / "eboot.c").read_text()
    # Everything from the flash-read helper to main(): the gzip reader and
    # copy_raw() itself, none of the boot/jump code.
    start = src.index("uint8_t read_flash_byte(")
    end = src.index("int main()")
    assert "int copy_raw(" in src[start:end]
    (build / "copy_raw.inc").write_text(src[start:end])
    uzlib = CORE / "tools" / "sdk" / "uzlib" / "src"
    exe = build / "eboot_harness"
    subprocess.run(
        [CC, "-O1", "-w", "-DRUNTIME_BITS_TABLES", f"-I{uzlib}", f"-I{build}",
         str(HERE / "eboot_harness.c"), str(uzlib / "tinflate.c"),
         str(uzlib / "tinfgzip.c"), str(uzlib / "crc32.c"),
         str(uzlib / "adler32.c"), "-o", str(exe)],
        check=True)

    def run(running: bytes, upload: bytes, stage=None):
        """Lay the flash out as the upload handler does, run the copy, and
        return (result code, stale reads, flash afterwards)."""
        if stage is None:
            stage = fwbuild.ota_stage_addr(len(running), len(upload),
                                           upload[:2] == b"\x1f\x8b")
        flash = bytearray(b"\xFF" * FLASH_SIZE)
        flash[:len(running)] = running
        flash[stage:stage + len(upload)] = upload
        # What lies above the app area must survive: EEPROM, rf-cal, WiFi.
        flash[fwbuild.ESP01_SKETCH_AREA:] = b"\x5A" * (
            FLASH_SIZE - fwbuild.ESP01_SKETCH_AREA)
        fin, fout = build / "in.bin", build / "out.bin"
        fin.write_bytes(flash)
        out = subprocess.run(
            [str(exe), str(fin), str(stage), str(len(upload)), str(fout)],
            check=True, capture_output=True, text=True).stdout
        m = re.search(r"res=(-?\d+) staleReads=(\d+)", out)
        return int(m.group(1)), int(m.group(2)), fout.read_bytes()

    return run


def _image(n: int, seed: int) -> bytes:
    """An image-shaped blob of `n` bytes that gzips about as well as the
    real image does (~70 %)."""
    rng = random.Random(seed)
    words = [bytes(rng.randrange(256) for _ in range(4)) for _ in range(1500)]
    out = bytearray(b"\xE9\x02\x00\x20")
    while len(out) < n:
        out += rng.choice(words) if rng.random() < 0.75 else bytes(
            rng.randrange(256) for _ in range(4))
    return bytes(out[:n])


def _assert_flashed(flash: bytes, image: bytes) -> None:
    assert flash[:len(image)] == image
    last = (len(image) + SECTOR - 1) & ~(SECTOR - 1)
    assert set(flash[len(image):last]) <= {0xFF}
    assert set(flash[fwbuild.ESP01_SKETCH_AREA:]) == {0x5A}


def test_eboot_unpacks_the_packed_image(harness):
    running = _image(453776, seed=1)
    new = _image(455000, seed=2)
    res, stale, flash = harness(running, fwbuild.ota_gzip_image(new))
    assert (res, stale) == (0, 0)
    _assert_flashed(flash, new)


def test_eboot_still_copies_a_plain_image(harness):
    running = _image(453776, seed=1)
    new = _image(455000, seed=2)
    res, stale, flash = harness(running, new)
    assert (res, stale) == (0, 0)
    _assert_flashed(flash, new)


def test_image_past_the_plain_ceiling_replaces_itself(harness):
    # Bigger than half the flash: only the packed upload fits beside it, and
    # the unpack writes across the region the upload was stored in.
    size = fwbuild.ota_ceiling(1.0) + 60000
    running, new = _image(size, seed=3), _image(size, seed=4)
    packed = fwbuild.ota_gzip_image(new)
    assert not fwbuild.ota_can_replace(size, size, size)
    assert fwbuild.ota_can_replace(size, size, len(packed))
    res, stale, flash = harness(running, packed)
    assert (res, stale) == (0, 0)
    _assert_flashed(flash, new)


def test_a_small_image_can_take_a_much_larger_one(harness):
    # The unpacked image runs far past where its packed copy starts.
    running = _image(300000, seed=5)
    new = _image(560000, seed=6)
    packed = fwbuild.ota_gzip_image(new)
    assert fwbuild.ota_can_replace(len(running), len(new), len(packed))
    res, stale, flash = harness(running, packed)
    assert (res, stale) == (0, 0)
    _assert_flashed(flash, new)


def test_harness_sees_a_corrupt_stream(harness):
    # The harness must be able to fail, or the tests above prove nothing.
    packed = bytearray(fwbuild.ota_gzip_image(_image(455000, seed=2)))
    packed[40000:40016] = bytes(16)
    res, _stale, flash = harness(_image(453776, seed=1), bytes(packed))
    assert res != 0 or flash[:455000] != _image(455000, seed=2)


def _front_loaded_image() -> bytes:
    """600 KB of zeros, then 100 KB of noise: the zeros pack into a few
    hundred bytes, so eboot has them all written out long before it reads
    the noise."""
    rng = random.Random(8)
    return (b"\xE9\x02\x00\x20" + bytes(600000) +
            bytes(rng.randrange(256) for _ in range(100000)))


def test_unpack_over_its_own_stored_copy_destroys_the_image(harness):
    # Why the follower refuses an image that would unpack across its stored
    # copy: stored right behind a small running image, this one is written
    # over input eboot has not read yet. (Also proves the harness can fail.)
    running = _image(8192, seed=7)
    new = _front_loaded_image()
    packed = fwbuild.ota_gzip_image(new)
    low = fwbuild.ota_stage_addr(len(running), len(running), False)
    assert low < len(new)
    res, stale, flash = harness(running, packed, stage=low)
    assert stale > 0
    assert res != 0 or flash[:len(new)] != new


def test_the_same_image_is_safe_where_the_follower_stores_it(harness):
    running = _image(8192, seed=7)
    new = _front_loaded_image()
    packed = fwbuild.ota_gzip_image(new)
    assert fwbuild.ota_can_replace(len(running), len(new), len(packed))
    res, stale, flash = harness(running, packed)
    assert (res, stale) == (0, 0)
    _assert_flashed(flash, new)


def test_follower_rule_refuses_what_would_overlap():
    # 700 KB unpacked against a 420 KB packed copy: they cannot both fit
    # without overlapping, whatever the running image is.
    assert not fwbuild.ota_can_replace(8192, 700000, 420000)


# --- what fwbuild's arithmetic mirrors ------------------------------------------

def test_app_area_matches_the_linker_script():
    if CORE is None:
        pytest.skip("needs the installed ESP8266 core")
    ini = (PROJECT / "platformio.ini").read_text()
    script = re.search(r"board_build\.ldscript\s*=\s*(\S+)", ini).group(1)
    ld = (CORE / "tools" / "sdk" / "ld" / script).read_text()
    fs_start = int(re.search(r"_FS_start\s*=\s*(0x[0-9A-Fa-f]+)", ld).group(1), 16)
    assert fs_start - 0x40200000 == fwbuild.ESP01_SKETCH_AREA


def test_max_upload_matches_the_upload_handler():
    web = (PROJECT / "FollowerWeb.cpp").read_text()
    assert "uint32_t maxSketchSpace = (freeSpace - 0x1000) & 0xFFFFF000;" in web
    # A gzip upload reserves only its own sectors and must unpack below them
    # (ota_stage_addr / ota_gzip_unpack_fits).
    assert "? otaGzipReserve(contentLen, maxSketchSpace)" in web
    assert "Update.begin(otaReservedBytes, U_FLASH)" in web
    assert "!otaGzipUnpackFits(otaGzipTail, appAreaBytes()," in web
    header = (PROJECT / "FollowerOtaImage.h").read_text()
    assert "return otaSectorCeil(n) <= appArea - reserved;" in header


def test_the_wall_link_download_stores_an_image_the_way_the_upload_does():
    """The download (wl.Update) is a second writer of the same flash session:
    same reserve, same unpack check, and never both at once."""
    update = (PROJECT / "FollowerUpdate.cpp").read_text()
    assert "offer.packed ? otaGzipReserve(offer.size, maxSpace) : maxSpace;" in update
    assert "Update.begin(reserved, U_FLASH)" in update
    assert "!otaGzipUnpackFits(tail, appAreaBytes(), reserved)" in update
    assert "followerUpdateFirstBytes(buf, got, offer.packed," in update
    # Judged before anything is erased.
    assert update.index("followerUpdateFirstBytes(") < update.index("Update.begin(")
    assert update.index("followerUpdateAnswer(") < update.index("fetch(offer, *body)")
    # HTTPClient connects a copy of the client it is given: the body is only
    # readable through its own stream.
    assert "http.getStreamPtr()" in update and "fetch(offer, conn)" not in update
    policy = (PROJECT / "FollowerUpdatePolicy.h").read_text()
    assert "(freeSketchSpace - OTA_FLASH_SECTOR) & ~(OTA_FLASH_SECTOR - 1)" in policy
    web = (PROJECT / "FollowerWeb.cpp").read_text()
    guard = web.index("if (updateDownloadActive()) {")
    assert web.index("otaUploadGate(") < guard < web.index("masterOtaUploadActive = true;")


def test_image_offset_matches_the_firmware_header():
    header = (PROJECT / "FollowerOtaImage.h").read_text()
    offset = int(re.search(r"OTA_GZIP_IMAGE_OFFSET\s*=\s*(\d+);", header).group(1))
    assert offset == fwbuild.OTA_GZIP_IMAGE_OFFSET
