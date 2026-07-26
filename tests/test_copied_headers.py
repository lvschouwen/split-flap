"""Header-sharing gate (#350, reworked by #408).

The v2 trees used to carry byte-identical COPIES of every pure-logic header,
policed here for drift. #408 replaced that with one real shared include:
`firmware/v2/shared/` is on every project's `-I` path (target AND native
envs), so those headers now exist once and cannot drift at all.

What is left to police:
- shadowing: no tree may reintroduce a private copy of a shared header, which
  would silently win over the shared one via the include path.
- note:      some copies are DELIBERATE trims/derivatives (a follower's
  reduced API index, the Rescue app's parse-only record reader). Those stay
  separate files; assert each still carries a copy note pointing readers at
  its source of truth. Body comparison is not attempted — the trims are
  hand-maintained by design.
"""

import re
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parent.parent

MASTER = REPO / "firmware/v2/Master"
FOLLOWER = REPO / "firmware/v2/FollowerEsp01"
RESCUE = REPO / "firmware/v2/Rescue"
UNIT = REPO / "firmware/v2/Unit"
SHARED = REPO / "firmware/v2/shared"

V2_TREES = [MASTER, FOLLOWER, RESCUE, UNIT]

# Headers that legitimately exist in multiple trees WITHOUT being copies.
TREE_LOCAL_HEADERS = {
    "BuildVersion.h",  # generated per-tree rev stamp (gitignored)
}

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


def test_shared_headers_are_not_shadowed():
    """#408: a private copy in a tree silently wins over shared/ via the
    include path, which is exactly the drift the move eliminated."""
    for shared in sorted(SHARED.glob("*.h")):
        for tree in V2_TREES:
            shadow = tree / shared.name
            assert not shadow.is_file(), (
                f"{_rel(shadow)} shadows {_rel(shared)} — the shared header is "
                f"the single source of truth; delete the tree copy"
            )


def test_no_undeclared_duplicate_headers_across_trees():
    """A header name appearing in two trees is either a declared trim
    (NOTED_COPIES) or an accident. Anything else should live in shared/."""
    declared = {c.name for _, c in NOTED_COPIES} | TREE_LOCAL_HEADERS
    seen = {}
    for tree in V2_TREES:
        for header in tree.glob("*.h"):
            seen.setdefault(header.name, []).append(tree)
    for name, trees in sorted(seen.items()):
        if len(trees) < 2 or name in declared:
            continue
        pytest.fail(
            f"{name} exists in {[t.name for t in trees]} but is neither a "
            f"declared trim nor in shared/ — move it to firmware/v2/shared/"
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
