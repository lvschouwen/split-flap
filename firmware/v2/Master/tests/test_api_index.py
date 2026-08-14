"""ApiIndex.h drift gate for the S3 master (#448).

The follower has had a bidirectional gate since #358; the master only ever
had the native test_api legend guard, which checks that the index is
well-formed and its legend is complete — not that it is COMPLETE with
respect to the routes actually registered. That one-directional check is
how POST /cluster/leave stayed served-but-undeclared.

This diffs the (method, path) pairs registered across the Web*.cpp TU family
against ApiIndex.h's API_ROUTES table. Pure text, no build needed.

Both directions fail the build:
  - served but neither indexed nor listed below  -> the #448 defect
  - indexed but not served                       -> a phantom route

Routes are registered in two forms: server.on("path", HTTP_X, ...) — which
spans lines for the upload handlers — and the SSE stream, which is an
AsyncEventSource constructed with its path and attached via addHandler().
"""

import re
from pathlib import Path

PROJECT = Path(__file__).resolve().parent.parent

METHOD_MAP = {"HTTP_GET": "GET", "HTTP_POST": "POST"}

ROUTE_RE = re.compile(r'server\.on\(\s*"([^"]+)"\s*,\s*(HTTP_GET|HTTP_POST)')
SSE_RE = re.compile(r'AsyncEventSource\s+\w+\(\s*"([^"]+)"\s*\)')
INDEX_RE = re.compile(r'\{"(GET|POST)",\s*"([^"]+)",')

# Served on purpose, and deliberately absent from the operator-facing index.
# Kept here rather than in ApiIndex.h so it costs the firmware nothing — it
# has no runtime consumer, only this gate. Two classes:
#
#   BROWSER UI — the HTML/CSS/JS/icon the web app loads for itself. GET / is
#       the page; POST / is the display-text API and IS indexed.
#   CLUSTER WIRE — server-to-server only: HMAC-signed, source-IP bound to the
#       leader (#313). An operator never calls these by hand, and documenting
#       them in the curl-facing index would invite exactly that.
#
# Adding a route to this list is a deliberate, reviewed act. Anything not in
# API_ROUTES and not here fails the gate.
UNDOCUMENTED = {
    ("GET", "/"),
    ("GET", "/index.html"),
    ("GET", "/style.css"),
    ("GET", "/script.js"),
    ("GET", "/md5.js"),
    ("GET", "/favicon.png"),
    ("POST", "/cluster/join"),
    ("POST", "/cluster/ping"),
    ("POST", "/cluster/render"),
    ("POST", "/cluster/member/update"),
}


def registered_routes():
    routes = set()
    for src in sorted(PROJECT.glob("Web*.cpp")):
        text = src.read_text()
        for path, method in ROUTE_RE.findall(text):
            routes.add((METHOD_MAP[method], path))
        for path in SSE_RE.findall(text):
            routes.add(("GET", path))
    return routes


def indexed_routes():
    return set(INDEX_RE.findall((PROJECT / "ApiIndex.h").read_text()))


def test_every_served_route_is_indexed_or_deliberately_excluded():
    reg = registered_routes()
    assert reg, "no routes parsed — the regex drifted from the code"
    undeclared = sorted(reg - indexed_routes() - UNDOCUMENTED)
    assert not undeclared, (
        f"served but undeclared in ApiIndex.h: {undeclared} — add them to "
        f"API_ROUTES, or to UNDOCUMENTED here if they are deliberately "
        f"not operator-facing"
    )


def test_index_declares_no_phantom_routes():
    phantom = sorted(indexed_routes() - registered_routes())
    assert not phantom, (
        f"listed in ApiIndex.h but never registered: {phantom}"
    )


def test_undocumented_list_has_no_stale_entries():
    """A route removed from the code must not linger here pretending to be
    a deliberate exclusion."""
    stale = sorted(UNDOCUMENTED - registered_routes())
    assert not stale, f"UNDOCUMENTED lists unregistered routes: {stale}"


def test_undocumented_and_indexed_are_disjoint():
    overlap = sorted(UNDOCUMENTED & indexed_routes())
    assert not overlap, (
        f"routes both indexed and listed as undocumented: {overlap}"
    )


def test_sse_stream_is_recognised():
    """/events is registered via addHandler, not server.on — if that parse
    ever breaks, test_index_declares_no_phantom_routes would fail for a
    bogus reason, so pin it directly."""
    assert ("GET", "/events") in registered_routes()
