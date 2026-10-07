"""The operator page (#574): its pure model under `node --test`, and the
bundler that joins web/ into the one document the master serves."""
import gzip
import pathlib
import shutil
import subprocess
import sys

import pytest

PROJECT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(PROJECT))
import web_bundle  # noqa: E402

# The spec's budget for the whole page, gzipped.
PAGE_BUDGET_GZ = 40 * 1024

node = pytest.mark.skipif(shutil.which("node") is None, reason="node is not installed")


@node
def test_the_pure_model_passes_its_node_tests():
    web_bundle.write_constants(PROJECT)
    done = subprocess.run(["node", "--test", "web/test/"], cwd=PROJECT,
                          capture_output=True, text=True)
    assert done.returncode == 0, done.stdout + done.stderr
    # An empty or misnamed test directory also exits 0.
    assert "\nℹ fail 0" in done.stdout and "\nℹ tests 0" not in done.stdout, done.stdout


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
        for line in source.splitlines():
            if not line.startswith("import "):
                continue
            target = line.split("from")[1].strip(" ;'\"")
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
