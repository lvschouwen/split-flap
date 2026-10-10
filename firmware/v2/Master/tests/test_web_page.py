"""The operator page (#574): its pure model under `node --test`, and the
bundler that joins web/ into the one document the master serves."""
import gzip
import json
import pathlib
import re
import shutil
import subprocess
import sys

import pytest

PROJECT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(PROJECT))
import web_bundle  # noqa: E402

# The spec's budget for the whole page, gzipped.
PAGE_BUDGET_GZ = 80 * 1024

node = pytest.mark.skipif(shutil.which("node") is None, reason="node is not installed")


@node
def test_the_pure_model_passes_its_node_tests():
    web_bundle.write_constants(PROJECT)
    # The files by name and the reporter by name: what a directory argument
    # means and which reporter is the default both differ between Node versions.
    files = sorted(str(p.relative_to(PROJECT)) for p in (PROJECT / "web" / "test").glob("*.test.js"))
    assert files, "no test files under web/test"
    done = subprocess.run(["node", "--test", "--test-reporter=tap", *files], cwd=PROJECT,
                          capture_output=True, text=True)
    assert done.returncode == 0, done.stdout + done.stderr
    # A file without a test in it also exits 0.
    assert "\n# fail 0" in done.stdout and "\n# tests 0" not in done.stdout, done.stdout


@node
def test_the_joined_script_parses(tmp_path):
    web_bundle.write_constants(PROJECT)
    script = tmp_path / "page.js"
    script.write_text(web_bundle.bundle_js(PROJECT / "web"), encoding="utf-8")
    done = subprocess.run(["node", "--check", str(script)], capture_output=True, text=True)
    assert done.returncode == 0, done.stderr


def test_every_module_under_web_is_in_the_bundle_or_is_a_test():
    web = PROJECT / "web"
    web_bundle.write_constants(PROJECT)
    on_disk = {p.relative_to(web).as_posix() for p in web.rglob("*.js")
               if "test" not in p.relative_to(web).parts}
    assert on_disk == set(web_bundle.MODULES)


def test_a_module_comes_after_what_it_imports():
    web = PROJECT / "web"
    web_bundle.write_constants(PROJECT)
    for at, name in enumerate(web_bundle.MODULES):
        source = (web / name).read_text(encoding="utf-8")
        for target in re.findall(r"^import\s[^;]*?from\s+'([^']+)';", source, re.MULTILINE):
            wanted = (web / name).parent.joinpath(target).resolve().relative_to(web).as_posix()
            assert wanted in web_bundle.MODULES[:at], f"{name} imports {wanted} before it is joined"


def test_two_modules_declaring_one_name_fail_the_build(tmp_path, monkeypatch):
    for name in web_bundle.MODULES:
        target = tmp_path / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text("export function same() {}\n" if name in web_bundle.MODULES[:2]
                          else "", encoding="utf-8")
    with pytest.raises(ValueError, match="already declared"):
        web_bundle.bundle_js(tmp_path)


def test_the_alphabet_comes_from_the_protocol_header():
    header = (PROJECT.parent / "shared" / "SplitFlapProtocol.h").read_text(encoding="utf-8")
    alphabet = header.split('#define SFP_ALPHABET "')[1].split('"')[0]
    assert f"export const ALPHABET = '{alphabet}';" in web_bundle.constants_js(PROJECT)


def test_the_page_is_one_document_within_its_budget():
    page = web_bundle.build_page(PROJECT)
    text = page.decode("utf-8")
    assert "/*STYLE*/" not in text and "/*SCRIPT*/" not in text
    assert "\nimport " not in text and "\nexport " not in text
    assert len(gzip.compress(page, 9)) <= PAGE_BUDGET_GZ


def test_the_page_never_parses_a_string_as_html():
    """Wire strings reach the page as text nodes only (spec, section 6)."""
    web = PROJECT / "web"
    for path in web.rglob("*.js"):
        source = path.read_text(encoding="utf-8")
        for banned in ("innerHTML", "outerHTML", "insertAdjacentHTML", "document.write"):
            assert banned not in source, f"{path.name} uses {banned}"


# --- the preview server -------------------------------------------------------

def _dev_server():
    sys.path.insert(0, str(PROJECT / "web"))
    import dev_server
    return dev_server


@pytest.mark.parametrize("command, headers, read_only, served", [
    ("GET", {"Host": "127.0.0.1:8088"}, False, True),
    ("GET", {"Host": "localhost:8088"}, True, True),
    # A name that was made to resolve to this machine.
    ("GET", {"Host": "evil.example:8088"}, False, False),
    ("POST", {"Host": "127.0.0.1:8088", "Origin": "http://127.0.0.1:8088"}, False, True),
    # Another page open in the same browser.
    ("POST", {"Host": "127.0.0.1:8088", "Origin": "https://evil.example"}, False, False),
    ("POST", {"Host": "127.0.0.1:8088"}, False, False),
    ("PUT", {"Host": "127.0.0.1:8088", "Origin": "http://127.0.0.1:9999"}, False, False),
    ("POST", {"Host": "127.0.0.1:8088", "Origin": "http://127.0.0.1:8088"}, True, False),
])
def test_the_preview_server_passes_on_only_its_own_pages_requests(command, headers, read_only,
                                                                  served):
    why = _dev_server().refusal(command, headers, 8088, read_only)
    assert (why is None) == served, why


# --- the preview server's saved documents ------------------------------------

def _fixtures():
    sys.path.insert(0, str(PROJECT / "web"))
    import fixtures
    return fixtures


def _reasons(header, enum):
    """The wire names of an enum's reasons, from its to-string switch."""
    source = (PROJECT / header).read_text(encoding="utf-8")
    return set(re.findall(r"case %s::\w+:\s*return \"([a-z-]+)\";" % enum, source))


@pytest.mark.parametrize("header, enum, key", [
    ("UnitVerdict.h", "UnitReason", "unit"),
    ("BoardVerdict.h", "BoardReason", "board"),
])
def test_the_faults_wall_shows_every_reason_the_master_knows(header, enum, key):
    docs = _fixtures().documents("faults")
    shown = {doc["verdict"]["reason"] for path, doc in docs.items()
             if path.startswith(f"/api/v2/{key}/")}
    known = _reasons(header, enum)
    assert known, f"no reasons read from {header}"
    assert shown == known


def test_a_fixture_reason_carries_the_level_the_master_gives_it():
    fixtures = _fixtures()
    for header, enum, table in (("UnitVerdict.h", "UnitReason", fixtures.UNIT_REASONS),
                                ("BoardVerdict.h", "BoardReason", fixtures.BOARD_REASONS)):
        source = (PROJECT / header).read_text(encoding="utf-8")
        names = dict(re.findall(r"case %s::(\w+):\s*return \"([a-z-]+)\";" % enum, source))
        switch = source.split("ReasonLevel(%s r) {" % enum)[1].split("\n}")[0]
        faults = {names[n] for n in re.findall(r"case %s::(\w+):" % enum,
                                               switch.split("VerdictLevel::Working;")[1]
                                               .split("VerdictLevel::Fault;")[0])}
        for reason, entry in table.items():
            wanted = "working" if reason == "working" else "fault" if reason in faults else "note"
            assert entry[0] == wanted, reason


def test_the_page_calls_a_fault_what_the_master_calls_a_fault():
    # A reason that applies without leading comes without its level.
    source = (PROJECT / "web" / "model" / "verdict.js").read_text(encoding="utf-8")
    listed = source.split("export const UNIT_FAULT_REASONS = [")[1].split("];")[0]
    faults = {reason for reason, entry in _fixtures().UNIT_REASONS.items() if entry[0] == "fault"}
    assert set(re.findall(r"'([a-z-]+)'", listed)) == faults


def test_the_fixtures_name_no_real_network():
    text = json.dumps(_fixtures().documents("faults"))
    for address in re.findall(r"\d+\.\d+\.\d+\.\d+", text):
        assert address.startswith("192.0.2."), address  # the range kept for documents


def test_the_preview_server_refuses_every_change_when_it_serves_fixtures():
    headers = {"Host": "127.0.0.1:8088", "Origin": "http://127.0.0.1:8088"}
    assert _dev_server().refusal("POST", headers, 8088, False, fixtures=True)
    assert _dev_server().refusal("GET", headers, 8088, False, fixtures=True) is None


@pytest.mark.parametrize("path, status", [
    ("/api/v2/wall", 200), ("/api/v2/history?limit=50", 200), ("/api/v2/board/nope", 404),
    ("/api/v2/log?kind=ram", 200),
])
def test_the_preview_server_answers_a_path_from_the_fixtures(path, status):
    docs = _fixtures().documents("faults")
    got, body, kind = _dev_server().fixture_answer(docs, path)
    assert got == status
    if status == 200 and kind == "application/json":
        json.loads(body)
