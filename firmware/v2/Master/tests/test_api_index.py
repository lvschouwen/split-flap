"""ApiIndex.h drift gate for the S3 master (#448).

The follower has had a bidirectional gate since #358; the master only ever
had the native test_api legend guard, which checks that the index is
well-formed and its legend is complete — not that it is COMPLETE with
respect to the routes actually registered. That one-directional check is
how a served route once stayed undeclared.

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

ROUTE_RE = re.compile(
    r'server\.on\(\s*(?:AsyncURIMatcher::exact\()?"([^"]+)"\)?\s*,\s*(HTTP_GET|HTTP_POST)')
# A route registered as a plain string also answers every path below it.
PREFIX_ROUTE_RE = re.compile(r'server\.on\(\s*"([^"]+)"\s*,\s*(HTTP_GET|HTTP_POST)')
SSE_RE = re.compile(r'AsyncEventSource\s+\w+\(\s*"([^"]+)"\s*\)')
# A route with a JSON body is its own handler object; its method is set on it
# right after.
JSON_RE = re.compile(
    r'new\s+AsyncCallbackJsonWebHandler\(\s*"([^"]+)".*?->setMethod\(HTTP_(POST|PUT)\)', re.S)
INDEX_RE = re.compile(r'\{"(GET|POST|PUT)",\s*"([^"]+)",')

# Served on purpose, and deliberately absent from the operator-facing index:
# what the browser loads for itself. Kept here rather than in ApiIndex.h so it
# costs the firmware nothing — it has no runtime consumer, only this gate.
# Adding a route to this list is a deliberate, reviewed act. Anything not in
# API_ROUTES and not here fails the gate.
UNDOCUMENTED = {
    ("GET", "/"),
    ("GET", "/favicon.png"),
}


def registered_routes():
    routes = set()
    for src in sorted(PROJECT.glob("Web*.cpp")):
        text = src.read_text()
        for path, method in ROUTE_RE.findall(text):
            routes.add((METHOD_MAP[method], path))
        for path in SSE_RE.findall(text):
            routes.add(("GET", path))
        for path, method in JSON_RE.findall(text):
            routes.add((method, path))
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
    """The stream is registered via addHandler, not server.on — if that parse
    ever breaks, test_index_declares_no_phantom_routes would fail for a
    bogus reason, so pin it directly."""
    assert ("GET", "/api/v2/stream") in registered_routes()


# The old page's routes, each replaced by /api/v2 (#576). One of them served
# again is a second way to do one thing.
RETIRED = ["/console", "/index.html", "/script.js", "/style.css", "/md5.js", "/events",
           "/reboot", "/stop", "/reset-wifi", "/reset-units", "/reflash-units",
           "/units/health", "/units/health/refresh", "/log", "/log/flash", "/status",
           "/system/stats", "/system/info", "/mqtt/discover"]


def test_no_retired_route_is_served():
    served = registered_routes()
    back = sorted(r for r in served if r[1] in RETIRED or r[1].startswith("/unit/")
                  or r == ("POST", "/"))
    assert not back, f"retired routes served again: {back}"


def test_no_route_swallows_the_routes_below_it():
    """`server.on("/api", ...)` also answers /api/v2/wall, whichever handler
    is registered first. A path with served routes below it must be
    registered with AsyncURIMatcher::exact."""
    prefix_routes = set()
    for src in sorted(PROJECT.glob("Web*.cpp")):
        for path, method in PREFIX_ROUTE_RE.findall(src.read_text()):
            prefix_routes.add((METHOD_MAP[method], path))
    swallowed = sorted(
        (method, path, other)
        for method, path in prefix_routes if path != "/"
        for other_method, other in registered_routes()
        if other_method == method and other.startswith(path + "/")
    )
    assert not swallowed, f"(method, route, route it swallows): {swallowed}"
