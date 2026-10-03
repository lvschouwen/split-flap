"""GET /api is served from a flash asset that build_assets.py renders out of
ApiIndex.h's tables (#519). The C++ builder beside those tables is the
reference: this compiles it on the host and requires the generated text to
match byte for byte, so the Python generator can never describe a different
index than the header does.

Also pins the three handlers that used to hold ~20 KB of the ESP-01's RAM to
their low-memory shape.
"""
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

PROJECT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(PROJECT))

import build_assets  # noqa: E402  (importable outside SCons by design)

HOST_MAIN = r"""
#include <cstdio>
#include "ApiIndex.h"
int main() {
  static char buf[API_JSON_CAP];
  size_t n = buildApiJson(buf, sizeof(buf));
  if (n == 0 || n >= sizeof(buf)) return 1;
  fwrite(buf, 1, n, stdout);
  return 0;
}
"""


def reference_json(tmp_path) -> bytes:
    gxx = shutil.which("g++")
    assert gxx, "g++ is required: it builds the reference the asset is checked against"
    src = tmp_path / "ref.cpp"
    exe = tmp_path / "ref"
    src.write_text(HOST_MAIN)
    subprocess.run([gxx, "-std=gnu++17", "-DUNIT_TEST", f"-I{PROJECT}",
                    str(src), "-o", str(exe)], check=True)
    return subprocess.run([str(exe)], check=True, capture_output=True).stdout


def test_generated_asset_is_byte_identical_to_the_cpp_builder(tmp_path):
    generated = build_assets.api_index_json(PROJECT).encode("utf-8")
    assert generated == reference_json(tmp_path)


def test_generated_asset_is_the_documented_shape():
    doc = json.loads(build_assets.api_index_json(PROJECT))
    assert set(doc) == {"routes", "notServed", "legend"}
    assert {"m", "p", "d"} == set(doc["routes"][0])
    assert ("GET", "/api") in {(r["m"], r["p"]) for r in doc["routes"]}
    assert "/cluster/promote" in doc["notServed"]
    assert "sb" in doc["legend"] and "minHeap" in doc["legend"]


def _code(name):
    text = (PROJECT / name).read_text()
    text = re.sub(r"//[^\n]*", "", text)  # no /* */ blocks carry code here
    return text


def _handler(code, route):
    start = code.index(f'server.on("{route}"')
    end = code.index("server.on(", start + 10)
    return code[start:end]


def test_api_is_served_from_flash_without_a_buffer():
    api = _handler(_code("FollowerWeb.cpp"), "/api")
    assert "API_INDEX_JSON, API_INDEX_JSON_LEN" in api
    assert "buildApiJson" not in api and "static char" not in api


def test_no_handler_keeps_a_large_static_buffer():
    # The two that did (6 KB for /api, 8 KB for /units/health) were the
    # largest objects in the firmware's RAM.
    code = _code("FollowerWeb.cpp")
    for m in re.finditer(r"static\s+char\s+\w+\[([^\]]+)\]", code):
        size = m.group(1).strip()
        assert size.isdigit() and int(size) <= 256, f"static buffer [{size}] in a handler"


def test_health_reply_buffer_is_sized_for_the_row():
    health = _handler(_code("FollowerWeb.cpp"), "/units/health")
    assert "followerHealthBufCap(displayWidth, UNITS_AMOUNT)" in health
    # operator new aborts on this core, so the handler asks the heap first.
    assert health.index("if (!heapCanHold(cap)) {") < health.index("new (std::nothrow) char[cap]")
    assert "503" in health


def test_log_is_streamed_from_the_ring():
    log = _handler(_code("FollowerWeb.cpp"), "/log")
    assert "readSinceInto(" in log and "beginResponseStream(" in log
    assert "String body" not in log and "String out" not in log
    # The stream's buffer is a throwing allocation: ask first, answer 503.
    assert log.index("if (!heapCanHold(streamBytes)) {") < log.index("beginResponseStream(")
