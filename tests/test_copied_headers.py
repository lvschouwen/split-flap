"""Header-sharing gate (#350, reworked by #408).

The v2 trees used to carry byte-identical COPIES of every pure-logic header,
policed here for drift. #408 replaced that with one real shared include:
`firmware/v2/shared/` is on every project's `-I` path (target AND native
envs), so those headers now exist once and cannot drift at all.

What is left to police:
- look-alike: two headers in different trees that share most of their code
  under different names are a copy nobody declared (#536: FollowerPolicy.h
  was one for months). Declare it in NOTED_COPIES or move it to shared/.
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
    (MASTER / "WifiPolicy.h", RESCUE / "RescueWifiPolicy.h"),
    (MASTER / "ApiIndex.h", FOLLOWER / "ApiIndex.h"),
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


# --- undeclared look-alikes (#536) --------------------------------------------

LOOKALIKE_MIN_LINES = 12   # below this a header is too small to judge
LOOKALIKE_RATIO = 0.4      # share of the smaller header found in the other


CPP_KEYWORDS = {
    "if", "else", "for", "while", "do", "switch", "case", "default", "return",
    "break", "continue", "inline", "static", "const", "constexpr", "struct",
    "enum", "class", "bool", "int", "long", "char", "void", "unsigned",
    "uint8_t", "uint16_t", "uint32_t", "uint64_t", "int8_t", "int16_t",
    "int32_t", "size_t", "true", "false", "nullptr", "sizeof", "template",
    "typename", "String",
}


def _code_lines(path: Path) -> set:
    """The header's logic as a set of 3-line shingles with every identifier
    blanked. A hand copy renames its functions, types and constants (that is
    how FollowerPolicy.h hid), so names cannot be what is compared; the shape
    of three consecutive statements is distinctive enough without them."""
    src = path.read_text(errors="replace")
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.DOTALL)
    src = re.sub(r"//[^\n]*", "", src)
    shapes = []
    for line in src.splitlines():
        line = re.sub(r"\s+", " ", line).strip()
        # Braces, includes and one-word lines say nothing about shared logic.
        if len(line) < 16 or line.startswith("#"):
            continue
        shapes.append(re.sub(
            r"[A-Za-z_]\w*",
            lambda m: m.group(0) if m.group(0) in CPP_KEYWORDS else "_",
            line))
    return {"\n".join(shapes[i:i + 3]) for i in range(len(shapes) - 2)}


def _exact_lines(path: Path) -> set:
    """Verbatim code lines: catches a copy that kept its names but was
    reassembled from several sources in a different order."""
    src = path.read_text(errors="replace")
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.DOTALL)
    src = re.sub(r"//[^\n]*", "", src)
    lines = (re.sub(r"\s+", " ", l).strip() for l in src.splitlines())
    return {l for l in lines if len(l) >= 16 and not l.startswith("#")}


def _tree_headers(tree: Path):
    return [h for h in sorted(tree.glob("*.h"))
            if h.name not in TREE_LOCAL_HEADERS
            and not h.name.endswith("Assets.h")
            and h.name != "ApiIndexAsset.h"]


def lookalike_pairs():
    declared = {frozenset((s, c)) for s, c in NOTED_COPIES}
    headers = sorted(h for tree in V2_TREES for h in _tree_headers(tree))
    measures = [{h: fn(h) for h in headers} for fn in (_code_lines, _exact_lines)]
    found = []
    for i, a in enumerate(headers):
        for b in headers[i + 1:]:
            if a.parent == b.parent or frozenset((a, b)) in declared:
                continue
            ratio = 0.0
            for code in measures:
                small = min(len(code[a]), len(code[b]))
                if small >= LOOKALIKE_MIN_LINES:
                    ratio = max(ratio, len(code[a] & code[b]) / small)
            if ratio >= LOOKALIKE_RATIO:
                found.append((_rel(a), _rel(b), round(ratio, 2)))
    return found


def test_no_undeclared_lookalike_headers():
    found = lookalike_pairs()
    assert not found, (
        "headers in different trees share most of their code — move the "
        f"logic to firmware/v2/shared/ or declare the trim: {found}")


def test_lookalike_detector_sees_a_renamed_copy(tmp_path, monkeypatch):
    """The detector must catch a copy whose identifiers were all renamed in
    their declarations but whose bodies survived — the FollowerPolicy.h
    shape. Build one from a real header and expect it flagged."""
    source = MASTER / "ClusterFollowerPolicy.h"
    fake_tree = tmp_path / "Fake"
    fake_tree.mkdir()
    body = source.read_text().replace("clusterFollower", "fakeMember")
    (fake_tree / "FakePolicy.h").write_text(body)
    this = __import__(__name__)
    monkeypatch.setattr(this, "V2_TREES", [MASTER, fake_tree])
    monkeypatch.setattr(this, "_rel", lambda p: p.name)
    names = {(a, b) for a, b, _ in lookalike_pairs()}
    assert ("FakePolicy.h", "ClusterFollowerPolicy.h") in names or \
        ("ClusterFollowerPolicy.h", "FakePolicy.h") in names
