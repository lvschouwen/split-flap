# splitflap TUI Polish Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give the working `splitflap` TUI the split-flap product identity (amber theme, flap-cell wall, tuned layout), a friendlier command bar (clock-mode guard, help overlay, autocomplete, richer feedback, persistent history), and clear the #449 deferred-minors ledger.

**Architecture:** All work is in `cli/` (Python, Textual ≥0.80, httpx). New pure modules (`format.py`, `flapwall.py`, `history.py`) carry the testable logic; widgets/app get thin wiring. No `splitflap_client` wire changes, no firmware changes. Spec: `docs/superpowers/specs/2026-08-08-splitflap-tui-polish-design.md`. Issues: #450 (visual), #451 (command bar), #452 (cleanup). Branch: `feat/tui-polish`.

**Tech Stack:** Python 3.11+, Textual, rich.text.Text, httpx, pytest + pytest-asyncio (`asyncio_mode = "auto"` — plain `async def` tests, no marker needed).

## Global Constraints

- **NEVER markup-parse board-supplied text.** Every widget keeps `markup=False`; every styled string is assembled via `rich.text.Text` with literal `.append()` calls; every `border_title` assignment goes through `widgets.border_text()` (Textual's setter markup-parses raw strings unconditionally).
- Palette (exact values, from the spec): amber `#FFB000` (identity/ok), red `#E05B4B` (errors/faults), dim gray secondary, near-black background.
- Any function parsing firmware JSON uses the tolerant `_int`/`_str`/`_opt` style — **never a raise-capable coercion** (#449's recurring defect class).
- Test command (run from repo root): `cd cli && .venv/bin/python -m pytest tests/ -q`. The suite must be green at the end of every task. The 98 pre-existing tests stay green except the two `wall.content` assertions Task 3 deliberately migrates (justification inline there).
- Commits: conventional style scoped to the issue (`feat(#450): …`, `fix(#452): …`). NO `Closes` keywords in commit bodies — they go in the PR body only (repo rule).
- Files stay under 800 lines; new logic gets its own small module rather than growing `app.py`/`widgets.py`.
- Textual facts already established in this codebase (do not rediscover): `Static` has no readable `.renderable` accessor (keep own buffers); `Widget.render_str` is a reserved name (never define it); `Input` inherits up/down scroll bindings (CommandInput.on_key already suppresses them).

## File Structure

| File | Role |
| --- | --- |
| `cli/splitflap_tui/format.py` (new) | pure humanizers: `human_size`, `human_duration` |
| `cli/splitflap_tui/flapwall.py` (new) | pure flap-cell Text builder: `wall_cells`, `wall_width_needed` |
| `cli/splitflap_tui/history.py` (new) | best-effort command-history persistence |
| `cli/splitflap_tui/splitflap.tcss` (new) | the one theme/CSS file |
| `cli/splitflap_tui/screens/help_screen.py` (new) | `:help` / `?` modal |
| `cli/splitflap_tui/widgets.py` | FlapWall body in `WallPanel`, cluster dots, stats humanized, `format_cmd_status`, suggester, stale CSS class |
| `cli/splitflap_tui/commands.py` | `CANONICAL_NAMES`, `HelpEntry`, `HELP` table |
| `cli/splitflap_tui/app.py` | CSS_PATH, sub_title, device_mode tracking, clock guard, help wiring, history wiring, cmd timing, gate wording |
| `cli/splitflap_tui/poller.py` | client-handoff lock, per-loop client reuse |
| `cli/splitflap_tui/screens/board_detail.py` | `_log_text` cap, client reuse |
| `cli/pyproject.toml` | package-data for `*.tcss`, ruff dev dep |
| `cli/README.md` | new commands/keys/history documented |

---

### Task 1: Humanizer helpers (#450)

**Files:**
- Create: `cli/splitflap_tui/format.py`
- Test: `cli/tests/test_format.py`

**Interfaces:**
- Produces: `human_size(n: int) -> str` (`182432 -> "182k"`, `512 -> "512"`, `1_800_000 -> "1.8M"`), `human_duration(s: int) -> str` (`45 -> "45s"`, `125 -> "2m 5s"`, `7500 -> "2h 5m"`, `271234 -> "3d 3h"`). Consumed by Tasks 5 and 6.

- [ ] **Step 1: Write the failing tests**

```python
# cli/tests/test_format.py
from splitflap_tui.format import human_duration, human_size


def test_human_size_small_passthrough():
    assert human_size(512) == "512"
    assert human_size(0) == "0"


def test_human_size_kilo_and_mega():
    assert human_size(182432) == "182k"
    assert human_size(186432) == "186k"
    assert human_size(1_800_000) == "1.8M"


def test_human_duration_tiers():
    assert human_duration(45) == "45s"
    assert human_duration(125) == "2m 5s"
    assert human_duration(7500) == "2h 5m"
    assert human_duration(271234) == "3d 3h"
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd cli && .venv/bin/python -m pytest tests/test_format.py -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'splitflap_tui.format'`

- [ ] **Step 3: Write the implementation**

```python
# cli/splitflap_tui/format.py
"""Pure human-readable formatting for the stats bar (#450). No I/O."""
from __future__ import annotations


def human_size(n: int) -> str:
    if n < 1000:
        return str(n)
    if n < 1_000_000:
        return f"{n // 1000}k"
    return f"{n / 1_000_000:.1f}M"


def human_duration(s: int) -> str:
    if s < 60:
        return f"{s}s"
    m, sec = divmod(s, 60)
    if m < 60:
        return f"{m}m {sec}s"
    h, m = divmod(m, 60)
    if h < 24:
        return f"{h}h {m}m"
    d, h = divmod(h, 24)
    return f"{d}d {h}h"
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd cli && .venv/bin/python -m pytest tests/test_format.py -v`
Expected: 3 PASS

- [ ] **Step 5: Commit**

```bash
git add cli/splitflap_tui/format.py cli/tests/test_format.py
git commit -m "feat(#450): human_size/human_duration helpers for the stats bar"
```

---

### Task 2: Flap-cell Text builder (#450)

**Files:**
- Create: `cli/splitflap_tui/flapwall.py`
- Test: `cli/tests/test_flapwall.py`

**Interfaces:**
- Produces: `wall_cells(rows: list[str] | None, text: str, max_width: int) -> Text | None` — a `rich.text.Text` rendering each display character as one amber cell (`▐g▌`), a 1-space gap between units, a dim `rowN` label per row; returns `None` when the widest row cannot fit in `max_width` (caller falls back to plain text) or when there is no data. `wall_width_needed(row_len: int) -> int` exposes the geometry for the fit test. Consumed by Task 3.
- Safety: built exclusively with `Text.append` — bracketed payloads (`[/]`, `[ICE 704]`) can never be markup-parsed, by construction.

- [ ] **Step 1: Write the failing tests**

```python
# cli/tests/test_flapwall.py
from splitflap_tui.flapwall import wall_cells, wall_width_needed


def test_one_cell_per_char_with_unit_gaps():
    t = wall_cells(["AB"], "", 80)
    assert t.plain == "row0  ▐A▌ ▐B▌"


def test_two_rows_render_stacked_with_labels():
    t = wall_cells(["AB", "C"], "", 80)
    assert t.plain == "row0  ▐A▌ ▐B▌\nrow1  ▐C▌"


def test_narrow_terminal_returns_none_for_fallback():
    # 8 units need 6 + 4*8 - 1 = 37 columns
    assert wall_width_needed(8) == 37
    assert wall_cells(["ABCDEFGH"], "", 36) is None
    assert wall_cells(["ABCDEFGH"], "", 37) is not None


def test_no_rows_falls_back_to_text_field():
    t = wall_cells(None, "HI", 80)
    assert t.plain == "row0  ▐H▌ ▐I▌"


def test_no_data_returns_none():
    assert wall_cells(None, "", 80) is None
    assert wall_cells([], "", 80) is None


def test_bracket_payload_stays_literal():
    t = wall_cells(None, "[/]", 80)
    assert "[" in t.plain and "/" in t.plain and "]" in t.plain
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd cli && .venv/bin/python -m pytest tests/test_flapwall.py -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'splitflap_tui.flapwall'`

- [ ] **Step 3: Write the implementation**

```python
# cli/splitflap_tui/flapwall.py
"""Pure flap-cell rendering for the wall mirror (#450).

Geometry comes from the SSE row strings alone (one cell per character —
the firmware pads each row to its width); rows=None means the board sent
only `text` (not leading a wall), rendered as a single row. Built with
Text.append exclusively so board payloads can never be markup-parsed."""
from __future__ import annotations

from rich.text import Text

AMBER = "#FFB000"
CELL_EDGE_STYLE = "#7a5500"
CELL_GLYPH_STYLE = f"bold {AMBER} on #332200"
LABEL_STYLE = "dim"

_CELL_W = 4        # "▐g▌" plus the 1-space unit gap
_LABEL_W = 6       # "rowN" ljust'ed


def wall_width_needed(row_len: int) -> int:
    if row_len == 0:
        return _LABEL_W
    return _LABEL_W + _CELL_W * row_len - 1


def wall_cells(rows: list[str] | None, text: str, max_width: int) -> Text | None:
    src = rows if rows else ([text] if text else [])
    if not src:
        return None
    if max(wall_width_needed(len(r)) for r in src) > max_width:
        return None
    out = Text()
    for i, row in enumerate(src):
        if i:
            out.append("\n")
        out.append(f"row{i}".ljust(_LABEL_W), style=LABEL_STYLE)
        for j, ch in enumerate(row):
            if j:
                out.append(" ")
            out.append("▐", style=CELL_EDGE_STYLE)
            out.append(ch or " ", style=CELL_GLYPH_STYLE)
            out.append("▌", style=CELL_EDGE_STYLE)
    return out
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd cli && .venv/bin/python -m pytest tests/test_flapwall.py -v`
Expected: 6 PASS

- [ ] **Step 5: Commit**

```bash
git add cli/splitflap_tui/flapwall.py cli/tests/test_flapwall.py
git commit -m "feat(#450): pure flap-cell Text builder with narrow-terminal fallback"
```

---

### Task 3: WallPanel renders flap cells (#450)

**Files:**
- Modify: `cli/splitflap_tui/widgets.py` (`WallPanel`, lines 68-80)
- Modify: `cli/tests/test_app.py` (lines 122, 128 — the two `wall.content` assertions)
- Test: `cli/tests/test_app.py` (new test appended)

**Interfaces:**
- Consumes: `wall_cells` from Task 2.
- Produces: `WallPanel.wall_text() -> str` — the logical (uncelled) display text, the stable accessor tests use from now on (same pattern as `ClusterStrip.cluster_text()`). `WallPanel.update_wall(rows, text, stale)` signature unchanged (callers in `app.py` untouched).

- [ ] **Step 1: Update the two existing assertions + add the failing test**

In `test_sse_malformed_and_bracketed_text_render_literally_not_stale` (test_app.py:122,128), replace:

```python
        assert "[/][/] MALFORMED" in wall.content     # literal, not parsed
```
with
```python
        # Task 3 (#450): cells interleave ▐▌ glyphs into rendered content, so
        # the literal-rendering invariant is now pinned on the logical text
        # accessor instead of the visual content.
        assert "[/][/] MALFORMED" in wall.wall_text()  # literal, not parsed
```
and the later `assert "[ICE 704]" in wall.content` with `assert "[ICE 704]" in wall.wall_text()`. (Justified test change: the invariant being tested — literal rendering, thread survives — is unchanged; only the accessor moves, because the visual content now legitimately interleaves cell glyphs between characters.)

Append the new test (reuses `fake_factory`/`CFG`/`STATUS` already defined in test_app.py):

```python
@pytest.mark.asyncio
async def test_wall_renders_flap_cells():
    app = SplitflapApp(CFG, client_factory=fake_factory)
    async with app.run_test() as pilot:
        await pilot.pause(0.3)
        wall = app.query_one("#wall", WallPanel)
        app.apply_display(DisplayEvent(text="HI", self_row=None, rows=None))
        await pilot.pause(0.05)
        assert wall.wall_text() == "HI"
        assert "▐H▌ ▐I▌" in wall.content        # cells, not a plain string
```

Add `from splitflap_client.events import DisplayEvent` to test_app.py's imports.

- [ ] **Step 2: Run to verify the new test fails**

Run: `cd cli && .venv/bin/python -m pytest tests/test_app.py -v`
Expected: `test_wall_renders_flap_cells` FAILS (`AttributeError: ... no attribute 'wall_text'`); the two edited tests also fail for the same reason.

- [ ] **Step 3: Implement in widgets.py**

Replace the whole `WallPanel` class with:

```python
class WallPanel(Static):
    """markup=False (#441 finding 1a): the wall renders board/firmware
    -supplied display text verbatim — a payload like "[/]" is valid content,
    not Rich console markup, and must never be parsed as such. #450: body is
    flap cells (flapwall.wall_cells) when the panel is wide enough, plain
    text otherwise; on_resize re-decides."""

    def __init__(self, **kw):
        kw.setdefault("markup", False)
        super().__init__(**kw)
        self._rows: list[str] | None = None
        self._text = ""

    def wall_text(self) -> str:
        return "\n".join(self._rows) if self._rows else self._text

    def update_wall(self, rows: list[str] | None, text: str, stale: bool) -> None:
        self._rows, self._text = rows, text
        self.border_title = border_text("wall [STALE]" if stale else "wall")
        self._refresh_body()

    def _refresh_body(self) -> None:
        body = self.wall_text()
        if not body:
            self.update("(no display data)")
            return
        # Pre-layout content_size is 0x0 — assume wide, on_resize corrects.
        width = self.content_size.width or 200
        cells = wall_cells(self._rows, self._text, width)
        self.update(cells if cells is not None else body)

    def on_resize(self, event) -> None:
        self._refresh_body()
```

Add to widgets.py imports: `from .flapwall import wall_cells`.

- [ ] **Step 4: Run the full suite**

Run: `cd cli && .venv/bin/python -m pytest tests/ -q`
Expected: all PASS (98 + new).

- [ ] **Step 5: Commit**

```bash
git add cli/splitflap_tui/widgets.py cli/tests/test_app.py
git commit -m "feat(#450): WallPanel renders flap cells with plain-text fallback"
```

---

### Task 4: Theme CSS + stale dimming + layout proportions (#450)

**Files:**
- Create: `cli/splitflap_tui/splitflap.tcss`
- Modify: `cli/splitflap_tui/app.py` (CSS_PATH; compose gives the Horizontal an id; stale class on wall)
- Modify: `cli/splitflap_tui/widgets.py` (stale CSS class in every `mark_stale`/`clear_stale`)
- Modify: `cli/pyproject.toml` (package-data)
- Test: `cli/tests/test_app.py`

**Interfaces:**
- Produces: `.stale` CSS class contract — every panel's `mark_stale()` adds it, `clear_stale()` removes it (border-title marker retained alongside). `WallPanel` joins via `add_class`/`remove_class` calls in `app.py` (it has no mark/clear methods; the app owns wall staleness).

- [ ] **Step 1: Write the failing test**

```python
# append to cli/tests/test_app.py
@pytest.mark.asyncio
async def test_stale_panels_get_css_class_and_recover():
    app = SplitflapApp(CFG, client_factory=fake_factory)
    async with app.run_test() as pilot:
        await pilot.pause(0.3)
        strip = app.query_one("#cluster-strip", ClusterStrip)
        units = app.query_one("#units", UnitsTable)
        log = app.query_one("#log", LogTail)
        wall = app.query_one("#wall", WallPanel)

        app.apply_disconnect("boom")
        app.apply_wall_stale()
        await pilot.pause(0.05)
        assert strip.has_class("stale") and units.has_class("stale")
        assert log.has_class("stale") and wall.has_class("stale")

        agg = StatusAggregate.from_json(STATUS)
        app.apply_status(agg, ClusterStatus.from_json(STATUS["cluster"]))
        app.apply_display(DisplayEvent(text="OK", self_row=None, rows=None))
        await pilot.pause(0.05)
        assert not strip.has_class("stale") and not units.has_class("stale")
        assert not log.has_class("stale") and not wall.has_class("stale")
```

- [ ] **Step 2: Run to verify it fails**

Run: `cd cli && .venv/bin/python -m pytest tests/test_app.py::test_stale_panels_get_css_class_and_recover -v`
Expected: FAIL on the first `has_class("stale")` assert.

- [ ] **Step 3: Implement**

Create `cli/splitflap_tui/splitflap.tcss`:

```css
/* splitflap.tcss — the amber board-strip identity (#450). One file, no
   inline styles elsewhere; board-text literalness is a Python concern
   (markup=False), never a CSS one. */
Screen {
    background: #111008;
}
Header {
    background: #1c1708;
    color: #FFB000;
}
#wall, #cluster-strip, #units, #log {
    border: round #FFB000 50%;
    border-title-color: #FFB000;
}
#wall {
    height: auto;
    padding: 0 1;
}
#cluster-strip {
    height: auto;
    max-height: 6;
    padding: 0 1;
}
#main-split {
    height: 1fr;
}
#units {
    width: 46;
}
#log {
    width: 1fr;
}
#stats {
    height: 1;
    padding: 0 1;
    color: #b8a060;
}
#cmd-status {
    height: 1;
    padding: 0 1;
}
.stale {
    opacity: 45%;
}
```

In `app.py`:
- Add `CSS_PATH = "splitflap.tcss"` as a class attribute on `SplitflapApp` (below `TITLE`).
- In `compose()`, change `with Horizontal():` to `with Horizontal(id="main-split"):`.
- In `apply_wall_stale()`, add `wall.add_class("stale")` after the border_title line; in `apply_display()`, add `self.query_one("#wall", WallPanel).remove_class("stale")` (fold into the existing query).

In `widgets.py`, in each of `ClusterStrip`/`UnitsTable`/`LogTail`:
- `mark_stale`: add `self.add_class("stale")`
- `clear_stale`: add `self.remove_class("stale")`

In `cli/pyproject.toml`, after the `[tool.setuptools]` table add:

```toml
[tool.setuptools.package-data]
splitflap_tui = ["*.tcss"]
```

- [ ] **Step 4: Run the full suite (CSS parse errors surface as app-boot failures in every run_test test)**

Run: `cd cli && .venv/bin/python -m pytest tests/ -q`
Expected: all PASS.

- [ ] **Step 5: Reinstall the editable stub so the CSS ships, and sanity-launch**

Run: `cd cli && .venv/bin/pip install --force-reinstall --no-deps -e . -q && echo done`
Expected: `done` (guards the packaged-data path; the TUI itself is bench-verified in Task 12).

- [ ] **Step 6: Commit**

```bash
git add cli/splitflap_tui/splitflap.tcss cli/splitflap_tui/app.py cli/splitflap_tui/widgets.py cli/pyproject.toml cli/tests/test_app.py
git commit -m "feat(#450): amber theme CSS, stale-panel dimming, layout proportions"
```

---

### Task 5: Cluster strip — aligned columns + health dot (#450)

**Files:**
- Modify: `cli/splitflap_tui/widgets.py` (`ClusterStrip.update_cluster`, lines 105-120)
- Test: `cli/tests/test_widgets.py` (new file)

**Interfaces:**
- Consumes: `ClusterStatus`/`ClusterMember` (fields: `host,self_row,row,plat,rev,joined,failures,suspect,degraded,rescue,update_blocked,render_stuck`).
- Produces: `member_dot_style(m: ClusterMember) -> str` (module-level pure function in widgets.py returning a style string: `#E05B4B` bad, `#D9A03F` warn, `#FFB000` ok). `cluster_text()` contract unchanged (plain string, one line per member, still contains host/rev/state — existing test_app assertion keeps passing).

- [ ] **Step 1: Write the failing tests**

```python
# cli/tests/test_widgets.py
from splitflap_client.models import ClusterMember, ClusterStatus
from splitflap_tui.widgets import DOT_BAD, DOT_OK, DOT_WARN, member_dot_style


def member(**over):
    d = {"host": "192.168.15.121", "row": 0, "col": 0, "width": 5,
         "joined": True, "failures": 0, "rev": "9f694dd", "plat": "esp01"}
    d.update(over)
    return ClusterMember.from_json(d)


def test_dot_ok_when_joined_and_clean():
    assert member_dot_style(member()) == DOT_OK


def test_dot_warn_on_suspect_or_update_blocked():
    assert member_dot_style(member(suspect=True)) == DOT_WARN
    assert member_dot_style(member(updateBlocked=True)) == DOT_WARN


def test_dot_bad_on_lost_degraded_rescue_or_stuck():
    assert member_dot_style(member(joined=False)) == DOT_BAD
    assert member_dot_style(member(degraded=True)) == DOT_BAD
    assert member_dot_style(member(rescue=True)) == DOT_BAD
    assert member_dot_style(member(renderStuck=True)) == DOT_BAD
```

- [ ] **Step 2: Run to verify they fail**

Run: `cd cli && .venv/bin/python -m pytest tests/test_widgets.py -v`
Expected: FAIL — `ImportError: cannot import name 'member_dot_style'`

- [ ] **Step 3: Implement in widgets.py**

Add module-level constants + function (near the top, after `border_text`):

```python
DOT_OK = "#FFB000"
DOT_WARN = "#D9A03F"
DOT_BAD = "#E05B4B"


def member_dot_style(m: ClusterMember) -> str:
    if not m.joined or m.degraded or m.rescue or m.render_stuck:
        return DOT_BAD
    if m.suspect or m.update_blocked:
        return DOT_WARN
    return DOT_OK
```

(Import `ClusterMember` alongside the existing `ClusterStatus` import.)

Replace `ClusterStrip.update_cluster` with:

```python
    def update_cluster(self, c: ClusterStatus) -> None:
        out = Text()
        plain_lines = []
        for m in c.members:
            flags = [f for f, on in (("SUSPECT", m.suspect),
                                     ("DEGRADED", m.degraded),
                                     ("RESCUE", m.rescue),
                                     ("UPD-BLOCKED", m.update_blocked),
                                     ("STUCK", m.render_stuck)) if on]
            who = "self" if m.self_row else m.host
            state = "joined" if m.joined else f"lost({m.failures})"
            line = (f"row{m.row}  {who:<16.16} {m.plat:<8} {m.rev:<10.10} "
                    f"{state}" + (" " + " ".join(flags) if flags else ""))
            if plain_lines:
                out.append("\n")
            out.append("● ", style=member_dot_style(m))
            out.append(line)                       # literal, never markup
            plain_lines.append("● " + line)
        if c.rollout_phase and c.rollout_phase != "idle":
            line = f"rollout: {c.rollout_phase} src={c.rollout_src or 's3'}"
            if plain_lines:
                out.append("\n")
            out.append(line)
            plain_lines.append(line)
        self._text = "\n".join(plain_lines) or "(cluster disabled)"
        self.update(out if plain_lines else "(cluster disabled)")
```

- [ ] **Step 4: Run the full suite (test_app's `"817e3a9" in cluster_text()` must still pass)**

Run: `cd cli && .venv/bin/python -m pytest tests/ -q`
Expected: all PASS.

- [ ] **Step 5: Commit**

```bash
git add cli/splitflap_tui/widgets.py cli/tests/test_widgets.py
git commit -m "feat(#450): cluster strip health dots + aligned columns"
```

---

### Task 6: Stats humanized, header sub_title, status-line feedback (#450 + #451)

**Files:**
- Modify: `cli/splitflap_tui/widgets.py` (`StatsBar.update_stats`; new `format_cmd_status`)
- Modify: `cli/splitflap_tui/app.py` (`apply_status` sub_title; `run_command` timing; `apply_cmd_result` signature)
- Test: `cli/tests/test_widgets.py`

**Interfaces:**
- Consumes: `human_size`/`human_duration` (Task 1).
- Produces: `format_cmd_status(text: str, duration_s: float | None, clock: str) -> Text` (pure; glyph `✓` amber / `⛔` red derived from the existing `⛔ ` prefix convention, which is stripped from the body to avoid doubling; dim `(N.N s)` suffix only when a duration is given). `SplitflapApp.apply_cmd_result(text: str, duration_s: float | None = None)` — existing single-arg callers (board-detail messages, "cancelled", gate rejections) keep working.

- [ ] **Step 1: Write the failing tests**

```python
# append to cli/tests/test_widgets.py
from splitflap_client.models import SystemStatsNow
from splitflap_tui.widgets import StatsBar, format_cmd_status


def test_format_cmd_status_success_line():
    t = format_cmd_status("ok — set text: 'HI'", 0.42, "22:03:41")
    assert t.plain == "✓ 22:03:41  ok — set text: 'HI' (0.4 s)"


def test_format_cmd_status_error_strips_prefix_and_keeps_body_literal():
    t = format_cmd_status("⛔ 409: [cluster] busy", 1.26, "09:00:00")
    assert t.plain == "⛔ 09:00:00  409: [cluster] busy (1.3 s)"


def test_format_cmd_status_without_duration():
    t = format_cmd_status("cancelled", None, "12:00:00")
    assert t.plain == "✓ 12:00:00  cancelled"


def test_stats_bar_humanizes():
    s = SystemStatsNow.from_json({"heap": 182432, "minHeap": 141000,
                                  "rssi": -54, "uptime": 271234,
                                  "i2cTx": 12043, "i2cErr": 0})
    bar = StatsBar()
    lines = []
    bar.update = lambda text: lines.append(text)      # capture, no app needed
    bar.update_stats(s, True)
    assert lines[-1] == ("connected · heap 182k (min 141k) · rssi -54 · "
                         "up 3d 3h · i2c 12043/0 err")
```

- [ ] **Step 2: Run to verify they fail**

Run: `cd cli && .venv/bin/python -m pytest tests/test_widgets.py -v`
Expected: new tests FAIL (`ImportError: cannot import name 'format_cmd_status'`).

- [ ] **Step 3: Implement**

widgets.py — add near `border_text` (import `human_duration, human_size` from `.format`):

```python
def format_cmd_status(text: str, duration_s: float | None, clock: str) -> Text:
    """Status-line Text (#451): glyph + timestamp + literal result body.
    The ⛔-prefix convention (execute() puts it on every error) drives the
    glyph and is stripped from the body so it doesn't render twice."""
    ok = not text.startswith("⛔")
    body = text[2:] if text.startswith("⛔ ") else text
    out = Text()
    out.append("✓" if ok else "⛔", style="#FFB000" if ok else "#E05B4B")
    out.append(f" {clock}  ", style="dim")
    out.append(body)                                  # literal, never markup
    if duration_s is not None:
        out.append(f" ({duration_s:.1f} s)", style="dim")
    return out
```

widgets.py — `StatsBar.update_stats` becomes:

```python
    def update_stats(self, s: SystemStatsNow, connected: bool) -> None:
        link = "connected" if connected else "DISCONNECTED — retrying"
        self.update(f"{link} · heap {human_size(s.heap)} "
                    f"(min {human_size(s.min_heap)}) · rssi {s.rssi} · "
                    f"up {human_duration(s.uptime)} · "
                    f"i2c {s.i2c_tx}/{s.i2c_err} err")
```

app.py:
- imports: `import time` and `from datetime import datetime`; add `format_cmd_status` to the `.widgets` import.
- `apply_status` gains (after `self.plat = ...`):

```python
        s = agg.settings
        role = "leading" if s.cluster_leading else (s.cluster_state or "standalone")
        self.sub_title = (f"{s.effective_device_name or 'wall'} · "
                          f"{s.device_mode or '-'} · {role} · rev {s.version}")
```

- `run_command` measures duration:

```python
    def run_command(self, parsed: ParsedCommand) -> None:
        def work() -> None:
            t0 = time.monotonic()
            try:
                with self.client_factory(self.config.board_url()) as client:
                    result = execute(parsed, client, self.config, self.client_factory)
            except SplitflapError as exc:
                result = f"⛔ {exc}"
            self.call_from_thread(self.apply_cmd_result, result,
                                  time.monotonic() - t0)
        self.run_worker(work, thread=True)
```

- `apply_cmd_result` becomes:

```python
    def apply_cmd_result(self, text: str, duration_s: float | None = None) -> None:
        self.query_one("#cmd-status", Static).update(
            format_cmd_status(text, duration_s, datetime.now().strftime("%H:%M:%S")))
```

- [ ] **Step 4: Run the full suite**

Run: `cd cli && .venv/bin/python -m pytest tests/ -q`
Expected: all PASS.

- [ ] **Step 5: Commit**

```bash
git add cli/splitflap_tui/widgets.py cli/splitflap_tui/app.py cli/tests/test_widgets.py
git commit -m "feat(#450,#451): humanized stats bar, header sub_title, glyph+duration status line"
```

---

### Task 7: Clock-mode guard on `:text` (#451)

**Files:**
- Modify: `cli/splitflap_tui/app.py` (`__init__`, `apply_status`, `dispatch_command`)
- Test: `cli/tests/test_app.py`

**Interfaces:**
- Produces: `SplitflapApp.device_mode: str` (`""` until the first successful poll, then `Settings.device_mode`). Guard rule: `parsed.name == "text"` AND `device_mode == "clock"` routes through the plain (y/n) `ConfirmModal` with the clock warning appended to the summary; any other mode (including unknown `""`) dispatches as before.

- [ ] **Step 1: Write the failing tests**

```python
# append to cli/tests/test_app.py
from splitflap_tui.commands import parse
from splitflap_tui.confirm import ConfirmModal


@pytest.mark.asyncio
async def test_text_in_clock_mode_asks_before_sending():
    app = SplitflapApp(CFG, client_factory=fake_factory)
    async with app.run_test() as pilot:
        await pilot.pause(0.3)                 # STATUS fixture: deviceMode=clock
        assert app.device_mode == "clock"
        app.dispatch_command(parse("text HI"))
        await pilot.pause(0.05)
        assert isinstance(app.screen, ConfirmModal)
        assert "clock" in app.screen.summary
        app.screen.action_no()                 # cancel; nothing sent
        await pilot.pause(0.05)


@pytest.mark.asyncio
async def test_text_outside_clock_mode_sends_directly():
    app = SplitflapApp(CFG, client_factory=fake_factory)
    async with app.run_test() as pilot:
        await pilot.pause(0.3)
        for mode in ("text", ""):              # explicit text mode + unknown
            app.device_mode = mode
            app.dispatch_command(parse("text HI"))
            await pilot.pause(0.1)
            assert not isinstance(app.screen, ConfirmModal)
```

- [ ] **Step 2: Run to verify they fail**

Run: `cd cli && .venv/bin/python -m pytest tests/test_app.py -k clock -v`
Expected: FAIL — `AttributeError: ... no attribute 'device_mode'`.

- [ ] **Step 3: Implement in app.py**

- `__init__`: add `self.device_mode = ""` next to `self.plat = PLAT_S3`.
- `apply_status`: add `self.device_mode = agg.settings.device_mode` (beside the `self.plat` assignment).
- `dispatch_command` becomes:

```python
    def dispatch_command(self, parsed: ParsedCommand) -> None:
        if not self._capability_gate(parsed):
            return
        clock_guard = parsed.name == "text" and self.device_mode == "clock"
        if parsed.tier in (TIER_KILL, TIER_ROUTINE) and not clock_guard:
            self.run_command(parsed)
            return
        typed = parsed.tier == TIER_TYPED
        token = self._typed_confirm_token(parsed) if typed else ""
        summary = parsed.summary
        if clock_guard:
            summary = (f"{parsed.summary}\n"
                       "wall is in clock mode — the clock reclaims the "
                       "display at the next minute tick.\n"
                       "Send anyway? (tip: `:mode text` first)")

        def on_result(confirmed: bool) -> None:
            if confirmed:
                self.run_command(parsed)
            else:
                self.apply_cmd_result("cancelled")

        self.push_screen(ConfirmModal(summary, typed=typed, token=token),
                         on_result)
```

- [ ] **Step 4: Run the full suite**

Run: `cd cli && .venv/bin/python -m pytest tests/ -q`
Expected: all PASS.

- [ ] **Step 5: Commit**

```bash
git add cli/splitflap_tui/app.py cli/tests/test_app.py
git commit -m "feat(#451): clock-mode guard — :text in clock mode asks before sending"
```

---

### Task 8: Help table + drift gate + help overlay (#451)

**Files:**
- Modify: `cli/splitflap_tui/commands.py` (`CANONICAL_NAMES`, `HelpEntry`, `HELP`)
- Create: `cli/splitflap_tui/screens/help_screen.py`
- Modify: `cli/splitflap_tui/app.py` (binding + `help` interception)
- Test: `cli/tests/test_commands.py` (drift gate), `cli/tests/test_screens.py` (overlay)

**Interfaces:**
- Produces: `CANONICAL_NAMES: tuple[str, ...]` (every `ParsedCommand.name` `parse()` can emit), `HelpEntry(usage, tier, blurb, example)` frozen dataclass, `HELP: list[HelpEntry]`. `HelpScreen` (ModalScreen, escape/q closes). App: `?` binding + `:` bar lines `help`/`?` open it.
- Drift gate: parsing every `HELP` entry's `example` must yield exactly the `CANONICAL_NAMES` set — a command added to `parse()` without a help entry (or vice versa) fails the test.

- [ ] **Step 1: Write the failing tests**

```python
# append to cli/tests/test_commands.py
from splitflap_tui.commands import CANONICAL_NAMES, HELP, parse


def test_help_examples_cover_every_command_exactly():
    names = {parse(e.example).name for e in HELP}
    assert names == set(CANONICAL_NAMES)


def test_help_entries_are_complete():
    for e in HELP:
        assert e.usage and e.tier and e.blurb and e.example
```

```python
# append to cli/tests/test_screens.py
import pytest
from splitflap_tui.app import SplitflapApp
from splitflap_tui.screens.help_screen import HelpScreen

# reuse the module's existing app fixture/factory conventions; if the module
# defines its own CFG/factory use those, otherwise import from test_app:
from tests.test_app import CFG, fake_factory


@pytest.mark.asyncio
async def test_question_mark_opens_help_and_escape_closes():
    app = SplitflapApp(CFG, client_factory=fake_factory)
    async with app.run_test() as pilot:
        await pilot.pause(0.3)
        await pilot.press("question_mark")
        assert isinstance(app.screen, HelpScreen)
        await pilot.press("escape")
        await pilot.pause(0.05)
        assert not isinstance(app.screen, HelpScreen)
```

(If `from tests.test_app import` fails under the repo's pytest rootdir config, duplicate the 15-line `fake_factory`+`CFG` block locally in test_screens.py instead — do not restructure conftest for this.)

- [ ] **Step 2: Run to verify they fail**

Run: `cd cli && .venv/bin/python -m pytest tests/test_commands.py tests/test_screens.py -v`
Expected: FAIL — `ImportError: cannot import name 'CANONICAL_NAMES'`.

- [ ] **Step 3: Implement**

commands.py — append after the tier constants:

```python
CANONICAL_NAMES = ("stop", "text", "mode", "notify", "set", "op", "gates",
                   "reboot", "reset-units", "addr", "promote",
                   "cluster-leave", "config")


@dataclass(frozen=True)
class HelpEntry:
    usage: str
    tier: str
    blurb: str
    example: str      # must parse; the drift test derives coverage from it


HELP: list[HelpEntry] = [
    HelpEntry("stop", TIER_KILL, "blank and halt the wall NOW (also ctrl+s)", "stop"),
    HelpEntry("text <display text>", TIER_ROUTINE, "render text on the wall", "text HELLO"),
    HelpEntry("mode text|clock", TIER_ROUTINE, "switch device mode", "mode clock"),
    HelpEntry("notify <dwell-s> <text>", TIER_ROUTINE, "show text, then revert", "notify 10 DOOR"),
    HelpEntry("set <field> <value>", TIER_CONFIRM, "write one settings field", "set deviceMode clock"),
    HelpEntry("op home|jog|identify|self-test|reset-odometer|offset <unit> [value]",
              TIER_CONFIRM, "run a unit maintenance op", "op home 1"),
    HelpEntry("gates <unit> <mask>", TIER_CONFIRM, "set unit feature-gate byte", "gates 1 0x03"),
    HelpEntry("reboot [board]", TIER_TYPED, "reboot the default or named board", "reboot"),
    HelpEntry("reset-units", TIER_TYPED, "power-cycle every unit", "reset-units"),
    HelpEntry("addr burn <unit> <target> | addr clear <unit>", TIER_TYPED,
              "provision/clear a unit I2C address", "addr clear 1"),
    HelpEntry("promote", TIER_TYPED, "promote this board to cluster leader", "promote"),
    HelpEntry("cluster leave", TIER_TYPED, "leave the cluster", "cluster leave"),
    HelpEntry("cluster config <host|row|col|width;...>", TIER_TYPED,
              "replace the cluster member table", "cluster config |1|0|16;"),
]
```

Create `cli/splitflap_tui/screens/help_screen.py`:

```python
from __future__ import annotations

from textual.app import ComposeResult
from textual.screen import ModalScreen
from textual.widgets import DataTable, Label

from ..commands import HELP


class HelpScreen(ModalScreen[None]):
    """Command reference (#451) — content generated from commands.HELP so it
    cannot drift from the parser (drift-gated in test_commands)."""

    BINDINGS = [("escape", "close", "close"), ("q", "close", "close")]

    def compose(self) -> ComposeResult:
        yield Label("commands — esc/q to close")
        yield DataTable(id="help-table")

    def on_mount(self) -> None:
        table = self.query_one("#help-table", DataTable)
        table.add_columns("command", "tier", "what")
        for e in HELP:
            table.add_row(e.usage, e.tier, e.blurb)

    def action_close(self) -> None:
        self.dismiss(None)
```

app.py:
- BINDINGS: add `("question_mark", "help", "Help")`.
- New action:

```python
    def action_help(self) -> None:
        self.push_screen(HelpScreen())
```

- `on_input_submitted`, right after the `event.input.display = False` / `set_focus` lines and before `parse()`:

```python
        if line.strip() in ("help", "?"):
            self.push_screen(HelpScreen())
            return
```

- Import: `from .screens.help_screen import HelpScreen`.

Also add to `splitflap.tcss` (keeps the modal on-theme):

```css
HelpScreen {
    align: center middle;
}
HelpScreen DataTable {
    max-height: 80%;
    border: round #FFB000 50%;
}
```

- [ ] **Step 4: Run the full suite**

Run: `cd cli && .venv/bin/python -m pytest tests/ -q`
Expected: all PASS.

- [ ] **Step 5: Commit**

```bash
git add cli/splitflap_tui/commands.py cli/splitflap_tui/screens/help_screen.py cli/splitflap_tui/app.py cli/splitflap_tui/splitflap.tcss cli/tests/test_commands.py cli/tests/test_screens.py
git commit -m "feat(#451): :help/? overlay generated from the command table, drift-gated"
```

---

### Task 9: Command-name autocomplete (#451)

**Files:**
- Modify: `cli/splitflap_tui/widgets.py` (`CommandInput.__init__`)
- Test: `cli/tests/test_widgets.py`

**Interfaces:**
- Produces: `SUGGEST_WORDS: list[str]` in widgets.py (first-token vocabulary incl. two-word cluster forms + `help`); `CommandInput` default `suggester` = `SuggestFromList(SUGGEST_WORDS, case_sensitive=True)` (overridable via kwargs like its other settings).

- [ ] **Step 1: Write the failing test**

```python
# append to cli/tests/test_widgets.py
from splitflap_tui.widgets import SUGGEST_WORDS, CommandInput


async def test_command_input_suggests_command_names():
    inp = CommandInput()
    assert inp.suggester is not None
    assert await inp.suggester.get_suggestion("clu") == "cluster leave"
    assert await inp.suggester.get_suggestion("st") == "stop"
    assert "help" in SUGGEST_WORDS
```

- [ ] **Step 2: Run to verify it fails**

Run: `cd cli && .venv/bin/python -m pytest tests/test_widgets.py -k suggests -v`
Expected: FAIL — `ImportError: cannot import name 'SUGGEST_WORDS'`.

- [ ] **Step 3: Implement in widgets.py**

```python
from textual.suggester import SuggestFromList

SUGGEST_WORDS = ["stop", "text", "mode", "notify", "set", "op", "gates",
                 "reboot", "reset-units", "addr", "promote",
                 "cluster leave", "cluster config", "help"]
```

In `CommandInput.__init__`, before `super().__init__(**kw)`:

```python
        kw.setdefault("suggester",
                      SuggestFromList(SUGGEST_WORDS, case_sensitive=True))
```

- [ ] **Step 4: Run the full suite**

Run: `cd cli && .venv/bin/python -m pytest tests/ -q`
Expected: all PASS.

- [ ] **Step 5: Commit**

```bash
git add cli/splitflap_tui/widgets.py cli/tests/test_widgets.py
git commit -m "feat(#451): inline command-name autocomplete in the : bar"
```

---

### Task 10: Persistent command history (#451)

**Files:**
- Create: `cli/splitflap_tui/history.py`
- Modify: `cli/splitflap_tui/app.py` (`__init__` param, `on_mount` load, `on_input_submitted` save)
- Test: `cli/tests/test_history.py`, `cli/tests/test_app.py`

**Interfaces:**
- Produces: `load_history(path: Path | None = None) -> list[str]`, `save_history(lines: list[str], path: Path | None = None) -> None` (both best-effort: any `OSError` → `[]`/no-op; default path `~/.config/splitflap/history`; last-100 cap on both ends). `SplitflapApp.__init__` gains `history_path: Path | None = None` keyword (None = default path).

- [ ] **Step 1: Write the failing tests**

```python
# cli/tests/test_history.py
from splitflap_tui.history import load_history, save_history


def test_roundtrip_and_cap(tmp_path):
    p = tmp_path / "history"
    save_history([f"cmd {i}" for i in range(150)], p)
    got = load_history(p)
    assert len(got) == 100
    assert got[-1] == "cmd 149"


def test_load_missing_file_returns_empty(tmp_path):
    assert load_history(tmp_path / "nope") == []


def test_save_into_unwritable_parent_is_silent(tmp_path):
    blocker = tmp_path / "file"
    blocker.write_text("x")                    # a FILE where a dir is needed
    save_history(["a"], blocker / "history")    # must not raise
    assert load_history(blocker / "history") == []
```

```python
# append to cli/tests/test_app.py
@pytest.mark.asyncio
async def test_history_persists_across_sessions(tmp_path):
    hist = tmp_path / "history"
    hist.write_text("mode clock\n")
    app = SplitflapApp(CFG, client_factory=fake_factory, history_path=hist)
    async with app.run_test() as pilot:
        await pilot.pause(0.3)
        cmd = app.query_one("#command", CommandInput)
        assert cmd.history == ["mode clock"]
        app.action_open_command()
        cmd.value = "mode text"
        await pilot.press("enter")
        await pilot.pause(0.1)
    assert "mode text" in hist.read_text().splitlines()
```

Add `from splitflap_tui.widgets import CommandInput` to test_app.py imports (extend the existing widgets import line).

- [ ] **Step 2: Run to verify they fail**

Run: `cd cli && .venv/bin/python -m pytest tests/test_history.py tests/test_app.py::test_history_persists_across_sessions -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'splitflap_tui.history'`.

- [ ] **Step 3: Implement**

```python
# cli/splitflap_tui/history.py
"""Best-effort command-history persistence (#451). Never raises: an
unwritable/unreadable path degrades to in-memory-only history."""
from __future__ import annotations

from pathlib import Path

DEFAULT_HISTORY_PATH = Path.home() / ".config" / "splitflap" / "history"
LIMIT = 100


def load_history(path: Path | None = None) -> list[str]:
    p = path or DEFAULT_HISTORY_PATH
    try:
        return [l for l in p.read_text().splitlines() if l.strip()][-LIMIT:]
    except OSError:
        return []


def save_history(lines: list[str], path: Path | None = None) -> None:
    p = path or DEFAULT_HISTORY_PATH
    try:
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text("\n".join(lines[-LIMIT:]) + "\n")
    except OSError:
        pass
```

app.py:
- Import: `from pathlib import Path` and `from .history import load_history, save_history`.
- `__init__` signature gains `history_path: Path | None = None`; store `self.history_path = history_path`.
- `on_mount`, before the config check: 

```python
        cmd = self.query_one("#command", CommandInput)
        cmd.history = load_history(self.history_path)
        cmd.reset_history_cursor()
```

- `on_input_submitted`, right after `event.input.remember(line)`:

```python
        save_history(event.input.history, self.history_path)
```

- [ ] **Step 4: Run the full suite**

Run: `cd cli && .venv/bin/python -m pytest tests/ -q`
Expected: all PASS.

- [ ] **Step 5: Commit**

```bash
git add cli/splitflap_tui/history.py cli/splitflap_tui/app.py cli/tests/test_history.py cli/tests/test_app.py
git commit -m "feat(#451): command history persists to ~/.config/splitflap/history"
```

---

### Task 11: Cleanup ledger (#452)

**Files:**
- Modify: `cli/splitflap_tui/poller.py` (client-handoff lock; per-loop client reuse)
- Modify: `cli/splitflap_tui/screens/board_detail.py` (`_log_text` cap; client reuse)
- Modify: `cli/splitflap_tui/app.py` (`_capability_gate` display name)
- Modify: `cli/pyproject.toml` (ruff dev dep)
- Test: `cli/tests/test_poller.py`, `cli/tests/test_screens.py`, `cli/tests/test_app.py`

**Interfaces:**
- Produces: `Poller._client_lock: threading.Lock` guarding `_sse_client` handoff (stop() can no longer miss a client created between its check and the assignment). Status/log loops each hold ONE `BoardClient` across cycles, retry once on a fresh client before reporting (so a board-closed keep-alive never flashes a false DISCONNECT), and close it on loop exit. `DISPLAY_NAMES = {"config": "cluster config", "cluster-leave": "cluster leave"}` in app.py for gate messages.

- [ ] **Step 1: Write the failing tests**

```python
# append to cli/tests/test_poller.py (reuse its existing fake-app helper
# conventions; the FakeClient below is self-contained)
import threading

from splitflap_tui.poller import Poller


class _CloseTrackingClient:
    def __init__(self):
        self.closed = threading.Event()

    def close(self):
        self.closed.set()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


class _NullApp:
    def call_from_thread(self, fn, *a):
        fn(*a)


def test_sse_client_created_after_stop_is_closed_immediately():
    """#452: the stop()-vs-reconnect TOCTOU — stop() fired while a fresh
    cycle was between factory() and the _sse_client assignment used to leak
    a client blocked in a long read past unmount."""
    client = _CloseTrackingClient()
    p = Poller(_NullApp(), lambda url: client, "http://x", 1.0, 1.0)
    p.stop()                     # stop FIRST
    p._sse_cycle()               # cycle races in afterwards
    assert client.closed.is_set()
    assert p._sse_client is None


def test_status_loop_reuses_one_client_and_retries_once(monkeypatch):
    made = []

    class _JsonClient(_CloseTrackingClient):
        def __init__(self, fail_first: bool):
            super().__init__()
            self.fail_first = fail_first

        def get_json(self, path):
            if self.fail_first:
                self.fail_first = False
                from splitflap_client.transport import Unreachable
                raise Unreachable("http://x" + path, OSError("keepalive"))
            if path == "/status":
                return {"settings": {"plat": "esp32s3"}}
            return {"enabled": False}

    def factory(url):
        c = _JsonClient(fail_first=(len(made) == 0))
        made.append(c)
        return c

    applied = []

    class _App:
        def call_from_thread(self, fn, *a):
            applied.append(fn.__name__)

        def apply_status(self, agg, cluster): ...
        def apply_disconnect(self, msg): ...

    app = _App()
    p = Poller(app, factory, "http://x", 1.0, 1.0)
    p._status_cycle()            # first client dies mid-flight -> retried fresh
    p._status_cycle()            # second cycle reuses the fresh client
    assert len(made) == 2        # NOT 3: retry made one, cycle 2 made none
    assert applied == ["apply_status", "apply_status"]   # no disconnect flash
```

```python
# append to cli/tests/test_screens.py
def test_board_detail_log_buffer_is_capped():
    from splitflap_tui.screens.board_detail import LOG_CAP_LINES, cap_log
    text = "\n".join(f"line {i}" for i in range(500))
    capped = cap_log(text)
    lines = capped.splitlines()
    assert len(lines) == LOG_CAP_LINES
    assert lines[-1] == "line 499"
```

```python
# append to cli/tests/test_app.py
@pytest.mark.asyncio
async def test_gate_message_uses_spoken_command_name():
    app = SplitflapApp(CFG, client_factory=fake_factory)
    async with app.run_test() as pilot:
        await pilot.pause(0.3)
        app.plat = "esp01"       # /cluster/config is S3-only
        app.dispatch_command(parse("cluster config a|1|0|5;"))
        await pilot.pause(0.05)
        status = app.query_one("#cmd-status", Static)
        # gate wording (#452): "cluster config", not the internal "config"
        assert app._last_cmd_result.startswith("⛔ cluster config:")
```

For that last assertion to be testable without scraping widget internals, `apply_cmd_result` stores `self._last_cmd_result = text` before formatting (one line, add it in this task). Add `from textual.widgets import Static` to test_app.py imports if not present.

- [ ] **Step 2: Run to verify they fail**

Run: `cd cli && .venv/bin/python -m pytest tests/test_poller.py tests/test_screens.py tests/test_app.py -v`
Expected: new tests FAIL (`AttributeError`/`ImportError`/`AssertionError` respectively).

- [ ] **Step 3: Implement**

poller.py — `__init__` adds `self._client_lock = threading.Lock()` and `self._status_client: BoardClient | None = None`, `self._log_client: BoardClient | None = None`.

`stop()` becomes:

```python
    def stop(self) -> None:
        self.stop_event.set()
        with self._client_lock:
            c, self._sse_client = self._sse_client, None
        if c:
            try:
                c.close()
            except Exception:
                pass
```

`_sse_cycle` client handoff becomes:

```python
    def _sse_cycle(self) -> None:
        c = self.factory(self.url)
        with self._client_lock:
            if self.stop_event.is_set():
                # stop() already ran and can't see this client — our job.
                try:
                    c.close()
                except Exception:
                    pass
                return
            self._sse_client = c
        try:
            with c:
                for event in display_events(c):
                    self._sse_backoff = 1.0
                    if self.stop_event.is_set():
                        return
                    self.app.call_from_thread(self.app.apply_display, event)
        finally:
            with self._client_lock:
                self._sse_client = None
            if not self.stop_event.is_set():
                self.app.call_from_thread(self.app.apply_wall_stale)
```

Status loop reuse (log loop identical shape with `_log_client`/`fetch_flash_log`):

```python
    def _fetch_status(self):
        if self._status_client is None:
            self._status_client = self.factory(self.url)
        c = self._status_client
        return (StatusAggregate.from_json(c.get_json("/status")),
                ClusterStatus.from_json(c.get_json("/cluster/status")))

    def _drop_status_client(self) -> None:
        c, self._status_client = self._status_client, None
        if c:
            try:
                c.close()
            except Exception:
                pass

    def _status_cycle(self) -> None:
        try:
            agg, cluster = self._fetch_status()
        except SplitflapError:
            # a board-closed keep-alive is not an outage: retry once fresh
            self._drop_status_client()
            agg, cluster = self._fetch_status()
        if self.stop_event.is_set():
            return
        self.app.call_from_thread(self.app.apply_status, agg, cluster)

    def poll_status(self) -> None:
        try:
            run_poll_loop(self.stop_event, self.poll_s,
                          self._status_cycle, self._status_error)
        finally:
            self._drop_status_client()
```

`_status_error` gains `self._drop_status_client()` as its first line (broken client never survives into the next cycle); `_log_error` likewise drops the log client.

board_detail.py — add module-level:

```python
LOG_CAP_LINES = 200


def cap_log(text: str) -> str:
    return "\n".join(text.splitlines()[-LOG_CAP_LINES:])
```

and in `_apply`: `self._log_text = cap_log(self._log_text + log_text)`. Client reuse: same `_fetch`/`_drop`/retry-once shape as poller status, with `self._client: BoardClient | None = None` created lazily in `_cycle`, dropped in `_report_error`, and dropped in `on_unmount` after `stop_event.set()`.

app.py — add near `_route_known_anywhere`:

```python
DISPLAY_NAMES = {"config": "cluster config", "cluster-leave": "cluster leave"}
```

and in `_capability_gate` use `DISPLAY_NAMES.get(parsed.name, parsed.name)` in the message. In `apply_cmd_result`, first line: `self._last_cmd_result = text` (and `self._last_cmd_result = ""` in `__init__`).

Unused imports: add `"ruff>=0.4"` to the `dev` extra in pyproject.toml, then:

Run: `cd cli && .venv/bin/pip install -q -e ".[dev]" && .venv/bin/ruff check --select F401,F841 splitflap_client splitflap_tui tests`
Remove every reported unused import/variable (edit, don't `--fix`, so each removal is looked at).

- [ ] **Step 4: Run the full suite**

Run: `cd cli && .venv/bin/python -m pytest tests/ -q`
Expected: all PASS (watch the existing post-stop-callback tests in test_poller.py — they pin the behavior this task must preserve).

- [ ] **Step 5: Commit**

```bash
git add cli/splitflap_tui/poller.py cli/splitflap_tui/screens/board_detail.py cli/splitflap_tui/app.py cli/pyproject.toml cli/tests/
git commit -m "fix(#452): poller client-handoff lock + reuse, board-detail log cap, gate wording, unused imports"
```

---

### Task 12: Docs sync, full verification, PR

**Files:**
- Modify: `cli/README.md`

- [ ] **Step 1: Update cli/README.md**

Sync the docs with what shipped: the `?`/`:help` overlay, autocomplete, clock-mode guard behavior, persistent history (`~/.config/splitflap/history`), the amber theme + flap-cell wall (one sentence each; match the README's existing tone/structure — read it first, edit surgically).

- [ ] **Step 2: Full suite + lint, from a clean state**

Run: `cd cli && .venv/bin/python -m pytest tests/ -q && .venv/bin/ruff check --select F401,F841 splitflap_client splitflap_tui tests`
Expected: all PASS, no lint findings.

- [ ] **Step 3: Commit + push**

```bash
git add cli/README.md
git commit -m "docs(#450): README — help overlay, history, theme, clock guard"
git push -u origin feat/tui-polish
```

- [ ] **Step 4: Live operator smoke (bench tier — needs the user's VPN reachable)**

Launch `splitflap` in a real terminal and verify: amber theme + flap-cell wall render; `?` opens help, esc closes; up-arrow recalls history from a previous run; `:text HI` in clock mode pops the guard modal; status line shows glyph/timestamp/duration. This is interactive — hand the checklist to the user if the wall shouldn't be poked right now, and note results in the PR.

- [ ] **Step 5: Review gate + PR**

Per project workflow: final review over the combined branch diff (Claude reviewer ∥ Codex cross-model — both caught disjoint bug classes last arc). Then:

```bash
gh pr create --title "TUI polish: amber identity, flap-cell wall, command-bar UX, cleanup" --body "$(cat <<'EOF'
## Summary
- amber theme + flap-cell wall mirror + tuned layout/density (#450)
- clock-mode guard, :help overlay + autocomplete, status-line feedback, persistent history (#451)
- deferred-minors ledger from #449's final review (#452)

Design: docs/superpowers/specs/2026-08-08-splitflap-tui-polish-design.md

Closes #450
Closes #451
Closes #452

## Test plan
- [ ] cli test suite green (pytest, CI cli-tests job)
- [ ] ruff F401/F841 clean
- [ ] live operator smoke on the wall (checklist in Task 12)
EOF
)"
```

---

## Self-review notes

- Spec coverage: §1 → Tasks 1-6; §2 → Tasks 6-10; §3 → Task 11; testing section → per-task tests + Task 12; README/docs → Task 12. The spec's "wall height fixed" lands in Task 4's CSS (`#wall { height: auto; }` + content-driven rows); "units width sized to columns" is the `width: 46` rule (addr 6 + st 3 + sx 5 + odo 7 + vmin 6 + gates 6 + flags 6 + paddings ≈ 46).
- Type consistency: `wall_cells(rows, text, max_width)` (Tasks 2/3), `format_cmd_status(text, duration_s, clock)` (Task 6), `apply_cmd_result(text, duration_s=None)` (Tasks 6/7/11), `load_history/save_history(path)` (Task 10), `cap_log(text)` (Task 11) — names match across tasks.
- Known judgment calls a reviewer should see: the two `wall.content` → `wall.wall_text()` test edits (Task 3, justified inline); the retry-once-on-fresh-client semantics (Task 11) exist to keep client reuse from regressing the disconnect UX.
