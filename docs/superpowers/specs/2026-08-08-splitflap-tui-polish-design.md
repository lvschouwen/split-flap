# splitflap TUI polish — design

**Date:** 2026-08-08
**Status:** approved design (follows the shipped `2026-08-08-splitflap-tui-design.md`; PR #449 merged as `34053f4`)
**Scope:** visual identity + layout, command-bar UX, deferred-minors cleanup for the `splitflap` TUI in `cli/`. No `splitflap_client` wire changes, no firmware changes.

## Goal

The TUI works end-to-end (epic #441) but reads as an unstyled default-Textual app. This arc makes it *neat*: the split-flap product identity (amber board-strip aesthetic), a wall mirror that looks like the wall, tuned layout/density, a friendlier command bar, and the small code-quality items deferred from #449's final review.

Out of scope (explicit): `--help`/argument parsing, the feeder daemon, OTA/follower-image-relay commands, one-shot `--json` mode (all v2 candidates), any firmware/API change.

## 1. Visual system & dashboard layout

**Theme.** One Textual CSS file, `cli/splitflap_tui/splitflap.tcss` (today the app has no CSS). Palette carries the web UI identity:

- primary/accent: amber `#FFB000` — panel borders, titles, wall cells, health-ok dots
- error/fault: red `#E05B4B`
- secondary text: dim gray; background near-black

**Staleness.** A stale panel dims (CSS class toggled by the existing `mark_stale`/`clear_stale`) *and* keeps the `[STALE]` border-title marker — existing tests continue to pin the marker; the dimming is additive.

**Layout (same panel set, tuned):**

```
 splitflap · wall @ leader .88 · clock · leading · rev 817e3a9
┌─ wall ────────────────────────────────────────────────────────┐
│  row1  ▐K▌▐O▌▐M▌▐T▌▐ ▌▐G▌▐O▌▐E▌▐D▌▐ ▌▐ ▌▐ ▌▐ ▌▐ ▌▐ ▌▐ ▌      │
│  row0  ▐B▌▐I▌▐E▌▐R▌▐ ▌                                        │
└───────────────────────────────────────────────────────────────┘
┌─ cluster ─────────────────────────────────────────────────────┐
│ ● row1  self   esp32s3  817e3a9  joined                       │
│ ● row0  .121   esp01    9f694dd  joined                       │
└───────────────────────────────────────────────────────────────┘
┌─ units ───────────────────────────┐┌─ log ────────────────────┐
│ addr st  sx  odo   vmin gates fl  ││ 21:58:12 render ok …     │
└───────────────────────────────────┘└──────────────────────────┘
 heap 182k (min 141k) · rssi -54 · up 3d 2h · i2c 12043/0 err
 ✓ 22:03:41  text KOMT GOED — ok (0.4 s)
```

- **Wall = flap cells** (approach chosen over styled-text and full-flap-module variants): a custom `FlapWall` widget renders each display character as a discrete amber-on-dark cell with a gap between units. Geometry derives purely from the SSE `rows` strings — one cell per character; the firmware already pads each row to its width. No new wire knowledge.
- **Narrow-terminal fallback:** if the widget's content width can't fit the cell rendering (16 units ≈ 54 cols + label/borders), it degrades to the current plain-text rendering. It never wraps cells.
- **Wall height is fixed** (row count + padding) — other panels flex, the wall never gets squeezed.
- **Cluster strip:** one aligned row per member with a colored health dot (amber ok / distinct colors for SUSPECT/DEGRADED/RESCUE/etc. flags), replacing the crammed single-line format.
- **Units : log split:** units table width sized to its columns; log takes the remainder (today it's an even split).
- **Stats bar humanized:** `182k` not `186432`, `3d 2h` not `271234s`; formatting lives in pure helpers.

**Rendering discipline (unchanged, load-bearing):** board-supplied text is NEVER markup-parsed — `markup=False` everywhere; all new styled output is assembled via `rich.text.Text` (styles applied programmatically around literal content); `border_title` assignments keep going through `border_text()`.

## 2. Command-bar UX

1. **Clock-mode guard.** `:text …` while the last successful poll reported `deviceMode=clock` routes through the existing `ConfirmModal`: *"wall is in clock mode — the clock reclaims the display at the next minute tick. Send anyway? (tip: `:mode text` first)"*. Warning-with-override, not a block. Unknown mode (no poll yet) passes through unchanged. (Gotcha from the first operator session, 2026-08-08.)
2. **Discoverability.** `:help` and a bare `?` binding open a help overlay (modal screen): every command with syntax, tier badge (routine / confirm / typed-confirm / kill) and a one-line description, **generated from the command table in `commands.py`** so it cannot drift. The `:` bar gets inline command-name autocomplete via Textual's `Suggester`.
3. **Feedback quality.** The status line echoes `✓/⛔ + timestamp + command summary + result + duration`. Error results keep the verbatim board body, rendered literally.
4. **History persistence.** Up/down recall (already shipped) persists: last 100 submitted lines written to `~/.config/splitflap/history`, loaded at startup. Best-effort — an unwritable config dir must never crash or block the app.

## 3. Cleanup ledger (deferred minors from #449's final review)

- `Poller` stop()-vs-reconnect TOCTOU race → `threading.Lock`.
- `BoardDetailScreen._log_text` unbounded growth → cap at last 200 lines.
- Per-cycle `BoardClient` construction in poll loops → reuse one client per loop, recreate on error.
- Remove unused imports; fix `cluster config` capability-gate message wording.

## Testing

TDD; the existing 98 tests stay green throughout. New coverage:

- `FlapWall` cell rendering as a **pure text/Text-building function** (geometry from row strings, unit gaps, narrow-terminal fallback threshold, literal handling of `[`/`]` payloads).
- Clock-guard dispatch: clock mode → modal; text mode → direct; unknown mode → direct.
- Help overlay content generated from the command table (a new command without help metadata fails the test — the anti-drift gate).
- History persistence round-trip + unwritable-dir tolerance.
- Stat humanizer helpers; cluster-strip row formatting.
- Poller lock behavior extends the existing post-stop-callback tests.

E2E tier: live operator smoke on the wall (leader `.88`, esp01 `.121`) — same procedure as the #441 bench verification.

## Issues / branch

One arc branch (`feat/tui-polish`) + one PR. Issues (filed before coding, `effort:`/`gain:` labels):
1. visual system — theme, layout, flap-cell wall, density (section 1)
2. command-bar UX — guard, help/autocomplete, feedback, history persistence (section 2)
3. cleanup — deferred minors ledger (section 3)
