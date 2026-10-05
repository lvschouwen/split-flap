"""No HTTP route may be answered by another route's handler.

ESPAsyncWebServer matches `server.on("/a", …)` against "/a" AND every
"/a/…" request, and the first registered handler that matches wins. So a
route registered before a longer one under the same path and method silently
takes its requests: `/log` answered `/log/flash` with the RAM ring, and the
1 MB persistent flash log was unreachable.

The gate walks every board's registrations in registration order and requires
the longer path to come first.
"""
import re
from pathlib import Path

V2 = Path(__file__).resolve().parent.parent / "firmware" / "v2"
ROUTE = re.compile(r'server\.on\(\s*"([^"]+)"\s*,\s*(HTTP_[A-Z]+)')


def _strip(text):
    return re.sub(r"//[^\n]*", "", text)


def _master_files():
    # Registration order is the order of the *Register(server) calls.
    core = _strip((V2 / "Master" / "WebEndpoints.cpp").read_text())
    order = re.findall(r"\b(web[A-Z][A-Za-z]*)Register\(server\);", core)
    assert order, "no web*Register(server) calls found in WebEndpoints.cpp"
    files = []
    for name in order:
        path = V2 / "Master" / (name[0].upper() + name[1:] + ".cpp")
        assert path.exists(), f"{name}Register has no {path.name}"
        files.append(path)
    return files


BOARDS = {
    "Master": _master_files,
    "FollowerEsp01": lambda: [V2 / "FollowerEsp01" / "FollowerWeb.cpp"],
    "Rescue": lambda: [V2 / "Rescue" / "RescueWeb.cpp"],
}


def _routes(files):
    routes = []
    for path in files:
        for m in ROUTE.finditer(_strip(path.read_text())):
            routes.append((m.group(1), m.group(2), path.name))
    return routes


def _same_method(a, b):
    return a == b or "HTTP_ANY" in (a, b)


def test_every_board_registers_routes():
    for board, files in BOARDS.items():
        assert len(_routes(files())) >= 5, f"{board}: route scan found almost nothing"


def test_no_route_is_shadowed_by_an_earlier_prefix():
    problems = []
    for board, files in BOARDS.items():
        routes = _routes(files())
        for i, (uri, method, where) in enumerate(routes):
            for later_uri, later_method, later_where in routes[i + 1:]:
                if later_uri.startswith(uri.rstrip("/") + "/") and uri != "/" and \
                        _same_method(method, later_method):
                    problems.append(
                        f"{board}: {method} {uri} ({where}) is registered before "
                        f"{later_uri} ({later_where}) and answers its requests")
    assert not problems, "\n".join(problems)
