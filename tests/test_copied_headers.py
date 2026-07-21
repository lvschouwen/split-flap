"""Copied-header drift gate (#350).

The v2 copy policy (CLAUDE.md): pure-logic headers are COPIES across the
v2 project trees, never shared includes. That prices in silent divergence —
a one-byte drift in ClusterHmac.h (crypto) or UnitProtocolHelpers.h (wire
protocol) is the failure class this gate exists for.

Three modes:
- identical: the copies must be byte-identical.
- defines:   every `#define` line must match (header comments may differ).
- note:      the copy is a deliberate trim/derivative — assert it still
             carries a copy note so a reader is pointed at the source of
             truth. Body comparison is not attempted (the trims are
             hand-maintained by design).

Adding a new copied header? Add it to the manifest below AND to the
CLAUDE.md copied-header list.
"""

import re
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parent.parent

MASTER = REPO / "firmware/v2/Master"
FOLLOWER = REPO / "firmware/v2/FollowerEsp01"
RESCUE = REPO / "firmware/v2/Rescue"

# Master <-> FollowerEsp01 pairs that are byte-identical by policy.
IDENTICAL_PAIRS = [
    (MASTER / name, FOLLOWER / name)
    for name in [
        "UnitHealth.h",
        "UnitProtocolHelpers.h",
        "WearPolicy.h",
        "DisplayWidth.h",
        "ClusterHmac.h",
        "ClusterForeign.h",
        "UnitVitals.h",
        "HeartbeatPolicy.h",
        "BootHomePlan.h",
    ]
]

# Pairs where the protocol constants are the contract; header comments are
# tree-specific by design.
DEFINE_PAIRS = [
    (MASTER / "TwibootProtocol.h", FOLLOWER / "TwibootProtocol.h"),
]

# Deliberate trims/derivatives: the copy must say so. (source, copy) —
# source existence is asserted so a rename can't silently orphan the note.
NOTED_COPIES = [
    (MASTER / "SlotRecord.h", RESCUE / "RescueSlotRecord.h"),
    (MASTER / "OtaService.h", RESCUE / "RescueOta.h"),
    (MASTER / "DeviceIdentity.h", RESCUE / "RescueIdentity.h"),
    (FOLLOWER / "FollowerCors.h", RESCUE / "RescueCors.h"),
    (MASTER / "MaintenancePolicy.h", FOLLOWER / "FollowerOps.h"),
    (MASTER / "ApiIndex.h", FOLLOWER / "ApiIndex.h"),
    (MASTER / "ClusterDigest.h", FOLLOWER / "FollowerCors.h"),
]

COPY_NOTE_RE = re.compile(r"\b(copy|copied|copies)\b", re.IGNORECASE)


def _rel(p: Path) -> str:
    return str(p.relative_to(REPO))


@pytest.mark.parametrize(
    "source, copy", IDENTICAL_PAIRS, ids=lambda p: _rel(p) if isinstance(p, Path) else p
)
def test_identical_copy(source, copy):
    assert source.is_file(), f"missing {_rel(source)} — update the #350 manifest"
    assert copy.is_file(), f"missing {_rel(copy)} — update the #350 manifest"
    assert source.read_bytes() == copy.read_bytes(), (
        f"{_rel(copy)} has drifted from {_rel(source)} — the copy policy "
        f"requires fixing both trees in the same change"
    )


@pytest.mark.parametrize(
    "source, copy", DEFINE_PAIRS, ids=lambda p: _rel(p) if isinstance(p, Path) else p
)
def test_define_lines_match(source, copy):
    def defines(p):
        return [l.rstrip() for l in p.read_text().splitlines() if l.startswith("#define")]

    assert source.is_file(), f"missing {_rel(source)} — update the #350 manifest"
    assert copy.is_file(), f"missing {_rel(copy)} — update the #350 manifest"
    assert defines(source) == defines(copy), (
        f"#define drift between {_rel(source)} and {_rel(copy)}"
    )


@pytest.mark.parametrize(
    "source, copy", NOTED_COPIES, ids=lambda p: _rel(p) if isinstance(p, Path) else p
)
def test_trimmed_copy_carries_note(source, copy):
    assert source.is_file(), f"missing source {_rel(source)} — update the #350 manifest"
    assert copy.is_file(), f"missing copy {_rel(copy)} — update the #350 manifest"
    head = "\n".join(copy.read_text().splitlines()[:20])
    assert COPY_NOTE_RE.search(head), (
        f"{_rel(copy)} lost its copy note — readers must be pointed at "
        f"{_rel(source)}"
    )
