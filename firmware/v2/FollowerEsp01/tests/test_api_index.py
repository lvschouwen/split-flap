"""ApiIndex.h drift gate (#358).

GET /api claims to be the follower's endpoint surface; this test keeps it
honest forever by diffing the (method, path) pairs registered in
FollowerWeb.cpp against ApiIndex.h's API_ROUTES table — pure text, no build
needed. The deliberate platform gaps (API_NOT_SERVED) must additionally
never collide with a registered route.
"""

import re
from pathlib import Path

PROJECT = Path(__file__).resolve().parent.parent

METHOD_MAP = {"HTTP_GET": "GET", "HTTP_POST": "POST"}

ROUTE_RE = re.compile(r'server\.on\(\s*"([^"]+)"\s*,\s*(HTTP_GET|HTTP_POST)')
INDEX_RE = re.compile(r'\{"(GET|POST)",\s*"([^"]+)",')
NOT_SERVED_RE = re.compile(r'API_NOT_SERVED\[\]\s*=\s*\{(.*?)\};', re.DOTALL)


def registered_routes():
    src = (PROJECT / "FollowerWeb.cpp").read_text()
    return {(METHOD_MAP[m], p) for p, m in ROUTE_RE.findall(src)}


def indexed_routes():
    src = (PROJECT / "ApiIndex.h").read_text()
    return set((m, p) for m, p in INDEX_RE.findall(src))


def not_served():
    src = (PROJECT / "ApiIndex.h").read_text()
    block = NOT_SERVED_RE.search(src)
    assert block, "API_NOT_SERVED array missing from ApiIndex.h"
    return set(re.findall(r'"([^"]+)"', block.group(1)))


def test_index_matches_registered_routes():
    reg = registered_routes()
    idx = indexed_routes()
    assert reg, "no server.on(...) literals parsed — regex drifted from the code"
    missing = sorted(reg - idx)
    stale = sorted(idx - reg)
    assert not missing and not stale, (
        f"GET /api index drift: missing from ApiIndex.h {missing}; "
        f"listed but not registered {stale}"
    )


def test_not_served_routes_are_actually_not_served():
    reg_paths = {p for _, p in registered_routes()}
    overlap = sorted(not_served() & reg_paths)
    assert not overlap, (
        f"routes listed as not-served but registered: {overlap} — "
        f"remove them from API_NOT_SERVED"
    )


def test_not_served_matches_spec_list():
    # The spec's "Not served, by design" list
    # (docs/superpowers/specs/2026-07-14-v2-esp01-follower-design.md).
    assert not_served() == {
        "/cluster/digest",
        "/cluster/promote",
        "/cluster/config",
        "/cluster/discover",
    }
