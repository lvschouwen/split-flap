"""Entry-point gate for the in-system twiboot update (#499).

The stage-1 boot-section write borrows twiboot's own `spm` instructions: the
unit firmware preloads SPMCSR, then jumps *at* a known `spm` opcode in the
fielded bootloader and regains control through a pending timer interrupt (the
interrupt-return trick — see the design spec). That only works if the fielded
image is exactly the one measured, so this test pins, straight from the
committed prebuilt hex:

  * the whole-image CRC32 (the unit-side stage-1 guard refuses anything else),
  * an `spm` opcode (0x95 0xE8, little-endian `e8 95`) at every borrow site,
  * page 7 (0x7F80-0x7FFF) empty, so stage 1 can write do_spm there with zero
    brick risk.

A toolchain bump or an accidental image change moves one of these and fails CI
here, rather than silently bricking a unit on the wall. The addresses and CRC
are facts of the deployed fleet, not of whatever a fresh build happens to emit
(see firmware/v2/Unit/equivalent-revs.txt discipline for the same idea applied
to unit images).

Falsification check (verify-guards-by-falsification rule): flip any asserted
byte or the CRC and this test must go red. Proven once during authoring.
"""

import pathlib
import zlib

import pytest

REPO = pathlib.Path(__file__).resolve().parents[1]
PREBUILT = (
    REPO
    / "firmware/v2/UnitBootloader/prebuilt/twiboot-atmega328p-16mhz.hex"
)

BOOT_SECTION_START = 0x7C00
BOOT_SECTION_LEN = 1024
PAGE_SIZE = 128

# The fielded image on all 16 S3-row units (#511 dump). Every stage-1 guard
# keys off this exact CRC32 (zlib/PNG, reflected 0xEDB88320).
FIELDED_CRC32 = 0x18173ADD
FIELDED_USED_BYTES = 890

# `spm` is opcode 0x9508; AVR flash is little-endian, so it reads `e8 95`.
SPM_OPCODE_LE = bytes((0xE8, 0x95))

# Borrow sites, as absolute flash addresses. Each is an `spm` immediately
# preceded by twiboot's own SPMCSR load, so a jump landing *on* the spm (not on
# the load) runs it with the SPMCSR value the unit firmware set itself.
SPM_SITES = (0x7E60, 0x7E86, 0x7EA6, 0x7EB2)

# Page 7 — the empty page stage 1 writes do_spm into.
PAGE7_START = 0x7F80


def _ihex_to_boot_section(path: pathlib.Path) -> bytes:
    """Flatten an Intel-hex file into the 1 KB boot section image (0xFF fill).

    Deliberately standalone (no import from ~/bench-bins) so the gate depends
    only on the repo.
    """
    img = bytearray(b"\xff" * BOOT_SECTION_LEN)
    base = 0
    for line in path.read_text().splitlines():
        line = line.strip()
        if not line.startswith(":"):
            continue
        raw = bytes.fromhex(line[1:])
        count, addr, rectype = raw[0], (raw[1] << 8) | raw[2], raw[3]
        if rectype == 0x04:  # extended linear address
            base = ((raw[4] << 8) | raw[5]) << 16
        elif rectype == 0x02:  # extended segment address
            base = ((raw[4] << 8) | raw[5]) << 4
        elif rectype == 0x00:  # data
            for i in range(count):
                off = base + addr + i - BOOT_SECTION_START
                if 0 <= off < BOOT_SECTION_LEN:
                    img[off] = raw[4 + i]
    return bytes(img)


@pytest.fixture(scope="module")
def boot_image() -> bytes:
    assert PREBUILT.is_file(), f"missing prebuilt bootloader hex: {PREBUILT}"
    return _ihex_to_boot_section(PREBUILT)


def test_fielded_crc32(boot_image: bytes) -> None:
    assert zlib.crc32(boot_image) == FIELDED_CRC32, (
        "prebuilt twiboot image CRC32 changed — the fielded fleet is on "
        f"{FIELDED_CRC32:#010x}; a new image needs a new update campaign, not "
        "a silent byte drift here"
    )


def test_image_size_unchanged(boot_image: bytes) -> None:
    assert len(boot_image.rstrip(b"\xff")) == FIELDED_USED_BYTES


def test_spm_opcode_at_every_borrow_site(boot_image: bytes) -> None:
    for site in SPM_SITES:
        off = site - BOOT_SECTION_START
        assert boot_image[off : off + 2] == SPM_OPCODE_LE, (
            f"no spm opcode at {site:#06x} — the stage-1 borrow jumps here"
        )


def test_page7_is_empty(boot_image: bytes) -> None:
    off = PAGE7_START - BOOT_SECTION_START
    assert boot_image[off : off + PAGE_SIZE] == b"\xff" * PAGE_SIZE, (
        "page 7 is not empty — stage 1 writes do_spm there and relies on it "
        "being blank (no brick risk)"
    )
