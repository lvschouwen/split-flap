"""EEPROM claimed-block table gate (#466).

test_eeprom_layout guards the Nano's EEPROM map by walking a table of every
claimed block. A table is only as good as its completeness: a block that is
declared in UnitEeprom.h and never added to the table is invisible to the
overlap check, to the headroom count, and to the mask-distinctness check —
which is the same class of hole #466 was filed about, moved one step back.

No host test can see a constant it was not told about, so this gate reads the
header instead and requires the convention to hold:

    a checksummed block declares BOTH <PREFIX>_BLOCK_LEN and
    <PREFIX>_CHECKSUM_MASK, and both must appear in test_eeprom_layout

That makes the table self-policing for anything following the convention, and
leaves evading it a deliberate act rather than an oversight.

Lives at the repo root on purpose: `firmware/v2/Unit/test` is excluded from
make_manifest.py's UNIT_SRC_PATHSPECS but the tree itself is not, so a gate
placed beside the tests it guards would move the Unit source head and cost a
bundle restage every time it changed.
"""

import re
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parent.parent

UNIT = REPO / "firmware/v2/Unit"
EEPROM_H = UNIT / "UnitEeprom.h"
ODOMETER_H = UNIT / "UnitOdometer.h"
LAYOUT_TEST = UNIT / "test/test_eeprom_layout/test_main.cpp"

# UnitOdometer.h carries masks of both kinds and only one kind belongs to the
# EEPROM map. Distinctness matters between blocks that share an address space:
# a wire reply is never read at an EEPROM offset, so it has nothing to collide
# with. Every mask there must be classified as one or the other, so a new one
# cannot arrive unclassified and quietly skip the array.
ODOMETER_EEPROM_MASKS = {
    "ODO_SLOT_CHECKSUM_MASK",  # per-slot integrity byte, on the EEPROM
}
ODOMETER_WIRE_MASKS = {
    "ODO_REPLY_CHECKSUM_MASK",  # SFP_CMD_GET_ODOMETER reply, on the I2C wire
}


def _defines(path: Path, suffix: str) -> set[str]:
    src = path.read_text()
    return set(re.findall(r"^#define\s+([A-Z0-9_]+%s)\b" % suffix, src, re.M))


def _prefixes(names: set[str], suffix: str) -> set[str]:
    return {n[: -len(suffix)] for n in names}


@pytest.fixture(scope="module")
def layout_test_src() -> str:
    return LAYOUT_TEST.read_text()


@pytest.fixture(scope="module")
def claimed_block_rows(layout_test_src: str) -> list[tuple[str, str, str]]:
    """The {base, len, name} rows of kClaimedBlocks."""
    table = re.search(
        r"kClaimedBlocks\[\]\s*=\s*\{(.*?)\n\};", layout_test_src, re.S
    )
    assert table, "kClaimedBlocks table not found in %s" % LAYOUT_TEST.name
    rows = re.findall(
        r'\{\s*([A-Za-z0-9_]+)\s*,\s*([A-Za-z0-9_]+)\s*,\s*"([^"]*)"\s*\}',
        table.group(1),
    )
    assert rows, "kClaimedBlocks parsed as empty"
    return rows


@pytest.fixture(scope="module")
def mask_array(layout_test_src: str) -> set[str]:
    arr = re.search(
        r"const uint8_t masks\[\]\s*=\s*\{(.*?)\};", layout_test_src, re.S
    )
    assert arr, "masks[] array not found in %s" % LAYOUT_TEST.name
    return set(re.findall(r"[A-Z0-9_]+_CHECKSUM_MASK", arr.group(1)))


def test_every_block_len_is_a_claimed_block(claimed_block_rows):
    """A block the table does not list cannot be checked for overlap."""
    declared = _defines(EEPROM_H, "_BLOCK_LEN")
    tabled = {row[1] for row in claimed_block_rows}
    missing = declared - tabled
    assert not missing, (
        "UnitEeprom.h declares %s but test_eeprom_layout's kClaimedBlocks does "
        "not list them — the overlap and headroom guards cannot see these "
        "blocks. Add a row per block." % sorted(missing)
    )


def test_every_odometer_mask_is_classified():
    """Forces the eeprom/wire split to be stated rather than assumed."""
    unclassified = _defines(ODOMETER_H, "_CHECKSUM_MASK") - (
        ODOMETER_EEPROM_MASKS | ODOMETER_WIRE_MASKS
    )
    assert not unclassified, (
        "%s in UnitOdometer.h is neither an EEPROM block mask nor a wire mask "
        "— classify it here so the distinctness gate below knows whether it "
        "shares an address space with the EEPROM blocks." % sorted(unclassified)
    )


def test_every_checksum_mask_is_in_the_mask_array(mask_array):
    """A mask the array does not list cannot be checked for distinctness."""
    declared = _defines(EEPROM_H, "_CHECKSUM_MASK") | ODOMETER_EEPROM_MASKS
    missing = declared - mask_array
    assert not missing, (
        "%s declared but absent from test_block_masks_are_distinct's masks[] "
        "— a block read at the wrong offset could validate against a "
        "duplicate mask and nothing would fail." % sorted(missing)
    )


def test_a_block_declares_both_a_length_and_a_mask():
    """The convention the two gates above key on.

    Every block proves itself by checksum (UnitEeprom.h's stated invariant),
    so a length without a mask is an unchecksummed block, and a mask without a
    length is a block the claimed-block table has no way to size.
    """
    lens = _prefixes(_defines(EEPROM_H, "_BLOCK_LEN"), "_BLOCK_LEN")
    masks = _prefixes(_defines(EEPROM_H, "_CHECKSUM_MASK"), "_CHECKSUM_MASK")
    assert not (lens - masks), (
        "%s declare a _BLOCK_LEN with no _CHECKSUM_MASK — every block must "
        "prove itself by checksum." % sorted(lens - masks)
    )
    assert not (masks - lens), (
        "%s declare a _CHECKSUM_MASK with no _BLOCK_LEN — the claimed-block "
        "table cannot size them." % sorted(masks - lens)
    )


# Every EE_* constant the layout defines today, and what it is. A new one
# fails this gate until it is classified here — which is the point: the
# convention gates above only see blocks that declare a _BLOCK_LEN and a
# _CHECKSUM_MASK, so a field added with neither would otherwise land silently.
EEPROM_CONSTANTS = {
    # geometry
    "EE_SIZE": "device size",
    "EE_RESERVED_BASE": "region boundary",
    "EE_ODO_RING_BASE": "region boundary",
    "EE_RESERVED_NEXT_FREE": "region boundary",
    # identity block
    "EE_LAYOUT_VERSION": "identity field",
    "EE_I2C_ADDRESS": "identity field",
    "EE_ID_FLAGS": "identity field",
    "EE_ID_CHECKSUM": "identity checksum",
    "EE_ID_BLOCK_LEN": "identity length",
    "EE_ID_FLAG_PROVISIONED": "identity flag bit",
    "EE_ID_CHECKSUM_MASK": "identity mask",
    # calibration block
    "EE_CAL_OFFSET": "calibration field",
    "EE_CAL_CHECKSUM": "calibration checksum",
    "EE_CAL_BLOCK_LEN": "calibration length",
    "EE_CAL_CHECKSUM_MASK": "calibration mask",
    # lifetime health block
    "EE_HEALTH_BASE": "health field",
    "EE_HEALTH_LEN": "health payload length",
    "EE_HEALTH_CHECKSUM": "health checksum",
    "EE_HEALTH_BLOCK_LEN": "health length",
    "EE_HEALTH_CHECKSUM_MASK": "health mask",
    # odometer ring init marker
    "EE_RING_INIT_VERSION": "ring marker field",
    "EE_RING_INIT_CHECKSUM": "ring marker checksum",
    "EE_RING_INIT_BLOCK_LEN": "ring marker length",
    "EE_RING_INIT_CHECKSUM_MASK": "ring marker mask",
}


def test_every_eeprom_constant_is_accounted_for():
    declared = set(re.findall(r"^#define\s+(EE_[A-Z0-9_]+)", EEPROM_H.read_text(), re.M))
    unclassified = declared - set(EEPROM_CONSTANTS)
    assert not unclassified, (
        "%s is defined in UnitEeprom.h but not classified in this gate. If it "
        "claims EEPROM bytes it needs a _BLOCK_LEN, a _CHECKSUM_MASK and a row "
        "in kClaimedBlocks; every block proves itself by checksum. Add it here "
        "once you have decided which it is." % sorted(unclassified)
    )
    removed = set(EEPROM_CONSTANTS) - declared
    assert not removed, (
        "%s is classified here but no longer defined in UnitEeprom.h — drop it "
        "so this inventory keeps meaning something." % sorted(removed)
    )


def test_the_gate_knows_the_blocks_that_exist_today(claimed_block_rows, mask_array):
    """Pins the parse itself: a regex that silently matched nothing would make
    every assertion above vacuously true."""
    assert len(claimed_block_rows) == 5
    assert len(mask_array) == 5
    assert {row[2] for row in claimed_block_rows} == {
        "identity",
        "calibration",
        "lifetime health",
        "odometer ring marker",
        "odometer ring",
    }
