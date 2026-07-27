"""Static gates on the Wall Console assets (#399), plus the node runner for
its pure-model tests (tests/console_pure.test.mjs).

Two of these gates exist because the audit that opened #396 measured the
defects they catch: seven dead element IDs, and a stylesheet whose accent
token had silently drifted from the approved one. A third pins the strict-CSP
rule the device depends on — it is an offline LAN box, so an asset that
reaches for a CDN does not degrade, it disappears.

Run with:
    pytest tests/
"""

from __future__ import annotations

import pathlib
import re
import shutil
import subprocess

import pytest

HERE = pathlib.Path(__file__).resolve().parent
DATA = HERE.parent / "data"
HTML = DATA / "console.html"
CSS = DATA / "console.css"
JS = DATA / "console.js"
DETAIL = DATA / "console-detail.js"

# IDs the console builds by concatenation rather than naming outright:
# byId("s-" + screen) and querySelector("#s-" + screen).
DYNAMIC_IDS = {"s-home", "s-list", "s-ctrl", "s-unit"}


def _read(path: pathlib.Path) -> str:
    return path.read_text(encoding="utf-8")


def _scripts() -> str:
    return _read(JS) + "\n" + _read(DETAIL)


def _html_ids(html: str) -> set[str]:
    return set(re.findall(r'\bid="([^"]+)"', html))


def _js_ids(js: str) -> set[str]:
    return set(re.findall(r'\bbyId\("([^"]+)"\)', js))


# --- the assets exist and are wired into the bake ---------------------------


def test_console_assets_are_present():
    for path in (HTML, CSS, JS, DETAIL):
        assert path.exists(), f"missing console asset: {path.name}"


def test_console_assets_are_baked_into_progmem():
    import sys

    sys.path.insert(0, str(HERE.parent))
    import build_assets

    names = {name for name, _, _ in build_assets.ASSETS}
    assert {"console.html", "console.css", "console.js",
            "console-detail.js"} <= names


def test_console_markup_loads_every_script_it_needs():
    html = _read(HTML)
    for src in ("md5.js", "console.js", "console-detail.js"):
        assert f'src="{src}"' in html, f"console.html does not load {src}"
    # console-detail.js uses console.js's helpers, so order is load-bearing.
    assert html.index('src="console.js"') < html.index('src="console-detail.js"')


# --- no dead IDs, in either direction (#398) ---------------------------------


def test_every_console_html_id_is_used():
    html = _read(HTML)
    js, css = _scripts(), _read(CSS)
    # An ID earns its place by being looked up, styled, or pointed at from
    # markup (a label's for=, an aria-labelledby, an aria-controls).
    pointed_at = set()
    for attr in ("for", "aria-labelledby", "aria-controls", "aria-describedby"):
        for value in re.findall(rf'{attr}="([^"]+)"', html):
            pointed_at.update(value.split())
    dead = {
        i for i in _html_ids(html)
        if i not in DYNAMIC_IDS
        and i not in pointed_at
        and f'"{i}"' not in js
        and f"#{i}" not in css
    }
    assert not dead, f"dead element IDs in console.html: {sorted(dead)}"


def test_every_id_the_console_looks_up_exists():
    missing = _js_ids(_scripts()) - _html_ids(_read(HTML)) - DYNAMIC_IDS
    assert not missing, f"console.js looks up IDs that do not exist: {sorted(missing)}"


def test_dynamic_ids_are_really_built_by_the_console():
    js = _scripts()
    assert '"s-" + ' in js, "DYNAMIC_IDS claims a prefix the console no longer builds"


# --- strict CSP: an offline LAN device fetches nothing ----------------------


def test_console_markup_references_no_external_asset():
    html = _read(HTML)
    assert "http://" not in html and "https://" not in html
    # Protocol-relative sources are the other way off-device.
    for attr in ("src", "href"):
        for value in re.findall(rf'{attr}="([^"]+)"', html):
            assert not value.startswith("//"), value


def test_console_stylesheet_pulls_in_no_remote_font_or_image():
    css = _read(CSS)
    assert "@import" not in css
    for url in re.findall(r"url\(([^)]*)\)", css):
        assert not url.strip("'\" ").startswith(("http", "//")), url


def test_console_script_only_talks_to_this_box_and_lan_members():
    js = _scripts()
    # The one absolute prefix is the member fan-out, built from a host the
    # firmware has already validated as a LAN target (clusterHostIsLanTarget).
    # The SVG namespace is an XML identifier, never fetched.
    absolute = [u for u in re.findall(r'"https?://[^"]*"', js)
                if u != '"http://www.w3.org/2000/svg"']
    assert absolute == ['"http://"'], absolute


# --- wire strings are text nodes only ---------------------------------------


def test_console_never_assigns_innerhtml():
    # Device names, member hosts and display text are all attacker-reachable
    # strings; the console must never route one through markup parsing.
    js = _scripts()
    assert "innerHTML" not in js
    assert "outerHTML" not in js
    assert "insertAdjacentHTML" not in js


def test_console_markup_carries_no_inline_handlers():
    html = _read(HTML)
    assert not re.search(r"\son[a-z]+=", html), "inline event handler in console.html"


# --- the #397 token law -----------------------------------------------------


def test_console_carries_the_approved_accent_and_fault_colours():
    css = _read(CSS)
    # #397 settled both: --amber #D9A03F is the approved accent (the dead
    # #FFB000 duplicate died with it), and --warn was lightened to #EA6E5C
    # because the old #E05B4B measured 4.26:1 on destructive controls.
    assert re.search(r"--amber:\s*#D9A03F\b", css)
    assert re.search(r"--warn:\s*#EA6E5C\b", css)
    assert "#FFB000" not in css
    assert "#E05B4B" not in css


def test_console_declares_each_colour_token_once():
    css = _read(CSS)
    declared = re.findall(r"^\s*(--[a-z0-9-]+):", css, flags=re.MULTILINE)
    dupes = {n for n in declared if declared.count(n) > 1}
    assert not dupes, f"token declared more than once: {sorted(dupes)}"


def test_console_has_no_second_theme():
    # #396 R3: one dark theme, a documented product decision.
    assert not re.search(r"@media[^{]*prefers-color-scheme", _read(CSS))


def test_console_honours_reduced_motion():
    assert "prefers-reduced-motion" in _read(CSS)


# --- one front door ---------------------------------------------------------


def test_console_does_not_reach_for_the_deleted_panel():
    # #399 replaced the five-tab panel outright. A link to it would 404, and
    # a capability parked behind such a link would be silently unreachable.
    js = _scripts()
    assert "/panel" not in js
    assert "/index.html" not in js
    assert "script.js" not in _read(HTML)
    assert "style.css" not in _read(HTML)


# --- no hand-copied firmware constants (#399) -------------------------------


def test_console_reads_unit_flag_bits_from_the_generated_constants():
    # A bare `u.fl & 2` is a copy of UNIT_FLAG_LAST_HOME_FAILED that no gate
    # would notice going stale. Every mask must come through SFP.flag.
    literal = re.findall(r"\bfl\s*&\s*(?!SFP\.)\w", _scripts())
    assert not literal, f"unit flag mask hard-coded in the console: {literal}"


def test_console_takes_its_geometry_from_the_generated_constants():
    js = _scripts()
    for magic in ("2038", "/ 45", "* 45"):
        assert magic not in js, f"drum geometry hard-coded in the console: {magic}"


# --- pure model -------------------------------------------------------------


@pytest.mark.skipif(shutil.which("node") is None, reason="node not installed")
def test_console_pure_model():
    result = subprocess.run(
        ["node", "--test", str(HERE / "console_pure.test.mjs")],
        cwd=HERE.parent, capture_output=True, text=True,
    )
    assert result.returncode == 0, result.stdout + result.stderr
