# Note-size flaps Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A parametric generator that writes the 64-flap Note-size set (and a test plate) as Bambu-ready 3mf plates.

**Architecture:** Pure geometry rules (alphabet, front/back pairing, display→flap mapping, plate packing) are plain Python and unit-tested. build123d builds each flap as a black body with flush inlay pockets plus one solid per colour. A stdlib 3mf writer emits Bambu project metadata so every part opens already assigned to an AMS slot.

**Tech Stack:** Python 3.12 (uv), build123d 0.13.0 (OCP 8.0.1.1.0), pytest 9.2.0, stdlib `zipfile`/`xml.etree`.

**Spec:** `docs/superpowers/specs/2026-11-20-note-size-flaps-design.md` (issue #670)

## Global Constraints

- All code under `mechanics/flaps/`; generated output to `mechanics/flaps/out/` (gitignored, never committed).
- Exact pins (#648): `build123d==0.13.0 pytest==9.2.0` in a dedicated venv `$CACHE/mech-venv`, **not** the shared check venv.
- `mech` tree runs only when `mechanics/` is touched or `check.sh mech` is asked for; a bare `check.sh` keeps today's trees.
- `mechanics/` joins the code-standard gate (`PY_ROOTS` in `scripts/check_standard.py`); new code meets the limits, the baseline only shrinks.
- Every dimension lives in `params.py` (`FlapParams`, frozen dataclass); no literal mm values elsewhere.
- Drum order is the 64-entry list in `alphabet.json`; character i: top half on the **front of flap i**, bottom half on the **back of flap i−1** (circular).
- AMS slots: 1 black, 2 white, 3 red, 4 orange, 5 yellow, 6 green, 7 blue, 8 violet.
- Font: Barlow Condensed SemiBold (OFL), `.ttf` + `OFL.txt` checked in under `mechanics/flaps/fonts/`, source URL and sha256 in `fonts/README.md`.
- Public repo: no LAN/personal data.
- Commits reference #670; the last one carries `Closes #670` only after Lucas confirms the Bambu import.

## Review Focus

1. **Back-face orientation:** an asymmetric glyph's bottom half must read upright on a fallen flap, not mirrored left-right → pinned by `test_back_half_keeps_x` (Task 2).
2. **Wrap-around pairing:** flap 63's back carries flap 0's (blank) bottom half; flap 56 (♥) carries red's bottom → `test_pairing_wraps_and_colour_flaps` (Task 2).
3. **Inlay escaping the outline** at a large cap height or a stroke offset (wide `W`, `@`) → `test_inlays_inside_outline` over every char at cap_frac 0.80 (Task 3).
4. **Parameter overrides that cannot print** (inlay ≥ half the thickness, tab reach < 1.5) must fail loudly, not emit a broken plate → `test_params_reject_unprintable` (Task 1).
5. **Plate overflow / prime-tower overlap** when params grow → `test_pack_respects_bed_tower_and_spacing` with a 40 mm flap (Task 4).

---

### Task 1: Scaffold, params, alphabet, check wiring

**Files:**
- Create: `mechanics/flaps/{__init__.py,params.py,alphabet.py,alphabet.json,README.md}`, `mechanics/flaps/fonts/{BarlowCondensed-SemiBold.ttf,OFL.txt,README.md}`, `mechanics/flaps/tests/{__init__.py,test_params.py,test_alphabet.py}`
- Modify: `scripts/check.sh` (tree `mech`), `scripts/check_standard.py:33` (`PY_ROOTS += "mechanics/"`), `.gitignore` (`mechanics/flaps/out/`), `tests/test_check_script.py`

**Interfaces:**
- Produces: `FlapParams` (frozen dataclass) with these fields and defaults:
  `width=36.0, height=31.0, thickness=1.0, narrow_width=32.5, narrow_height=16.0, tab_y0=1.2, tab_h=1.2, inlay_depth=0.4, cap_frac=0.70, edge_margin=1.0, stroke_offset=0.0, bed=256.0, bed_margin=5.0, spacing=3.0, prime_tower=35.0, tess_tol=0.02`; properties `tab_reach = (width-narrow_width)/2`, `pivot_y = tab_y0 + tab_h/2`, `char_height = 2*height`; `FlapParams.with_overrides(**kw) -> FlapParams`.
- Produces: `Entry(kind: Literal["blank","char","heart","colour"], value: str)`; `load_alphabet(path: Path = DEFAULT) -> tuple[Entry, ...]`; `COLOUR_SLOTS: dict[str,int]` (the slot map above, keys `black white red orange yellow green blue violet`).

- [ ] **Step 1: Write failing tests**

```python
def test_defaults_match_spec():
    p = FlapParams()
    assert (p.width, p.height, p.thickness, p.inlay_depth) == (36.0, 31.0, 1.0, 0.4)
    assert p.tab_reach == 1.75 and p.pivot_y == 1.8

def test_params_reject_unprintable():
    for kw in ({"inlay_depth": 0.5}, {"narrow_width": 34.0}, {"cap_frac": 0.0}):
        with pytest.raises(ValueError):
            FlapParams().with_overrides(**kw)   # inlay >= t/2; tab_reach < 1.5; cap_frac outside (0, 0.9]

def test_alphabet_is_the_note_set():
    a = load_alphabet()
    assert len(a) == 64 and len(set(a)) == 64
    assert a[0] == Entry("blank", "") and a[1].value == "A" and a[26].value == "Z"
    assert "".join(e.value for e in a[27:37]) == "1234567890"
    assert "".join(e.value for e in a[37:56]) == "!@#$()-+&=;:'\"%,./?"
    assert a[56] == Entry("heart", "")
    assert [e.value for e in a[57:]] == ["red","orange","yellow","green","blue","violet","white"]

def test_alphabet_rejects_bad_files(tmp_path):
    # parametrise: 63 entries, a duplicate entry, kind "glyph", colour "pink" -> each raises ValueError
```

In `tests/test_check_script.py`:
```python
def test_mech_tree_only_on_touch_or_by_name():
    src = CHECK.read_text()
    assert re.search(r"^ALL_TREES=\(unit master rescue follower py\)$", src, re.M)
    assert re.search(r"mechanics/\*\) want\[mech\]=1", src)
    assert "build123d==0.13.0" in src
```

- [ ] **Step 2: Run, expect FAIL** — `cd mechanics/flaps && ~/.cache/split-flap/mech-venv/bin/python -m pytest -q tests/` (create the venv by hand once: `uv venv --python 3.12 ~/.cache/split-flap/mech-venv && uv pip install --python …/bin/python build123d==0.13.0 pytest==9.2.0`); expected: import errors.

- [ ] **Step 3: Implement** `params.py`, `alphabet.py`, `alphabet.json` (list of `{"kind":…, "value":…}` in the order above). Validation in `FlapParams.__post_init__`. Fetch the font from `https://github.com/google/fonts/raw/main/ofl/barlowcondensed/BarlowCondensed-SemiBold.ttf` and `…/OFL.txt`; write their sha256 into `fonts/README.md`.
  In `check.sh`: `KNOWN_TREES=(unit master rescue follower py mech)` for name validation, `ALL_TREES` unchanged; `MECH_VENV="$CACHE/mech-venv"`, `MECH_PINS=(build123d==0.13.0 pytest==9.2.0)` with the same stamp logic as `ensure_venv` (generalise it to `ensure_venv <dir> <pins…>`); `run_mech() { step "mechanics pytest" "$ROOT/mechanics/flaps" "$MECH_VENV/bin/python" -m pytest -q tests/; }`; `touched_trees` maps `mechanics/*` → `mech` and loops over `KNOWN_TREES`; the tree loop iterates `KNOWN_TREES`.

- [ ] **Step 4: Run, expect PASS** — the command from Step 2, plus `scripts/check.sh py mech`; expected: all green, code standard gate green.

- [ ] **Step 5: Commit** — `feat(mechanics): flap params, Note alphabet and the mech check tree (#670)`

---

### Task 2: Pairing and display→flap mapping

**Files:** Create `mechanics/flaps/layout.py`, `mechanics/flaps/tests/test_layout.py`

**Interfaces:**
- Consumes: `Entry`, `load_alphabet`, `FlapParams`.
- Produces: `Face = Literal["front","back"]`; `faces_of_flap(i: int, alphabet) -> tuple[Entry, Entry]` = (top half of `alphabet[i]` on front, bottom half of `alphabet[(i+1) % n]` on back); `display_to_flap(x: float, y: float, p: FlapParams) -> tuple[Face, float, float]`.

The mapping (display origin = character centre on the midline, +y up; flap origin = hinge edge centre, +y away from hinge):
```python
def display_to_flap(x, y, p):
    if y >= 0:
        return "front", x, p.pivot_y + y
    return "back", x, p.pivot_y - y      # x is NOT mirrored: rotation is about the hinge (x) axis
```

- [ ] **Step 1: Failing tests**

```python
def test_pairing_wraps_and_colour_flaps():
    a = load_alphabet()
    assert faces_of_flap(0, a) == (a[0], a[1])
    assert faces_of_flap(63, a) == (a[63], a[0])
    assert faces_of_flap(56, a) == (Entry("heart",""), Entry("colour","red"))

def test_front_half_offsets_by_pivot():
    assert display_to_flap(3.0, 10.0, FlapParams()) == ("front", 3.0, 11.8)

def test_back_half_keeps_x():
    assert display_to_flap(-4.0, -10.0, FlapParams()) == ("back", -4.0, 11.8)

def test_midline_is_front():
    assert display_to_flap(0.0, 0.0, FlapParams())[0] == "front"
```

- [ ] **Step 2: Run, expect FAIL** — `… -m pytest -q tests/test_layout.py`
- [ ] **Step 3: Implement** the two functions in `layout.py`.
- [ ] **Step 4: Run, expect PASS**
- [ ] **Step 5: Commit** — `feat(mechanics): front/back pairing and display-to-flap mapping (#670)`

---

### Task 3: Flap geometry (outline, glyph halves, pockets)

**Files:** Create `mechanics/flaps/geometry.py`, `mechanics/flaps/tests/test_geometry.py`

**Interfaces:**
- Consumes: Task 1 + 2.
- Produces: `flap_outline(p) -> Sketch` (narrow zone `0..narrow_height` at `narrow_width`, tabs `tab_y0..tab_y0+tab_h` out to `width`, full `width` above, to `height`); `inlay_region(p) -> Sketch` (outline inset by `edge_margin`, excluding `y < pivot_y`); `glyph_display(entry, p) -> Sketch | None` (centred on the origin: `char` → `Text(value, font_size, font_path=FONT)` scaled so cap height = `cap_frac*char_height`, then `offset(stroke_offset)`; `heart` → parametric heart of the same cap height; `colour` → a rectangle covering the whole display window; `blank` → `None`); `half_on_flap(entry, half: Literal["top","bottom"], p) -> Sketch | None` (intersect with y≥0 / y<0, map with the transform `display_to_flap` implies, i.e. translate by `pivot_y` / mirror y then translate, then intersect with `inlay_region`); `FlapParts(body: Part, inlays: dict[str, Part])`; `build_flap(i, alphabet, p) -> FlapParts`.
- Build rule: front face sits at z=0 (printed on the plate), back face at z=thickness. Front pocket = front sketch extruded `0..inlay_depth`; back pocket = back sketch extruded `thickness-inlay_depth..thickness`. Body = outline extrusion minus both pockets. Inlay colour key: `white` for char/heart, the colour name for colour entries. Parts never overlap.

- [ ] **Step 1: Failing tests**

```python
def test_outline_bbox():          # width 36, height 31; narrow zone measured at y=8 is 32.5 wide
def test_volume_is_conserved():   # for flaps 1 ('A'), 56, 60: body.volume + Σinlays == outline.area*thickness (rel 1e-4)
def test_inlays_inside_outline(): # for every flap at cap_frac 0.80: each inlay bbox within outline bbox; no inlay point at y < pivot_y
def test_blank_flap_front_has_no_inlay():   # flap 0: front empty, back = bottom of 'A' (white)
def test_colour_flap_parts():     # flap 57: inlays keys == {"red","orange"}; flap 63: {"white"} (back is blank)
def test_asymmetric_glyph_halves(): # 'L' (flap 12): top-half inlay sits left of centre on the front; on flap 11's back the bottom stroke spans right of centre too (x not mirrored)
def test_heart_is_white():        # flap 56 inlays has "white" (front heart) and "red" (back)
```

- [ ] **Step 2: Run, expect FAIL**
- [ ] **Step 3: Implement** `geometry.py`. Measure the cap height from the rendered `H` bounding box once per font size (cache it) and scale `font_size` from that. Heart: two arcs plus a V, built from `cap_frac*char_height`.
- [ ] **Step 4: Run, expect PASS**; `scripts/check.sh mech` green (standard gate included).
- [ ] **Step 5: Commit** — `feat(mechanics): flap body, split glyph inlays and colour faces (#670)`

---

### Task 4: Plate packing

**Files:** Create `mechanics/flaps/plates.py`, `mechanics/flaps/tests/test_plates.py`

**Interfaces:**
- Produces: `Placed(flap: int, x: float, y: float)` (flap footprint min corner on the bed); `pack(flaps: Sequence[int], p, tower: bool) -> list[list[Placed]]` (grid fill in rows; footprint = `width × height` plus `spacing`; usable area = bed minus `bed_margin` all round, minus a `prime_tower` square at the top-right corner when `tower`); `split_by_colour(alphabet) -> tuple[list[int], list[int]]` (flaps whose two faces use only white or none vs the rest).

- [ ] **Step 1: Failing tests**

```python
def test_split_by_colour():
    plain, coloured = split_by_colour(load_alphabet())
    assert coloured == list(range(56, 64)) and len(plain) == 56

def test_pack_respects_bed_tower_and_spacing():   # p = FlapParams().with_overrides(width=40.0, narrow_width=36.5)
    # every Placed footprint inside [bed_margin, bed-bed_margin]^2, no two footprints closer than spacing,
    # none intersects the tower square when tower=True; every flap placed exactly once

def test_default_plate_counts():
    plain, coloured = split_by_colour(load_alphabet())
    # cols = (256-2*5+3)//(36+3) = 6, rows = (256-2*5+3)//(31+3) = 7 -> 42 per plate
    assert [len(pl) for pl in pack(plain, FlapParams(), tower=False)] == [42, 14]
    assert len(pack(coloured, FlapParams(), tower=True)) == 1
```

- [ ] **Step 2: Run, expect FAIL**
- [ ] **Step 3: Implement** `plates.py` (pure, no build123d import).
- [ ] **Step 4: Run, expect PASS**
- [ ] **Step 5: Commit** — `feat(mechanics): plate packing with prime-tower keep-out (#670)`

---

### Task 5: Bambu 3mf writer

**Files:** Create `mechanics/flaps/threemf.py`, `mechanics/flaps/tests/test_threemf.py`

**Interfaces:**
- Consumes: `FlapParts`, `Placed`, `COLOUR_SLOTS`.
- Produces: `write_plate(path: Path, placed: list[tuple[Placed, FlapParts]], p, bambu: bool = True) -> None`.
- Layout: `[Content_Types].xml`, `_rels/.rels`, `3D/3dmodel.model` (unit `millimeter`; one mesh object per part, named `flap{NN}_{body|<colour>}`; one assembly object per flap with `<components>` referencing its parts; one `<build><item>` per flap with a translation transform to its bed position), and with `bambu=True`, `Metadata/model_settings.config` (`<config><object id=…><metadata key="name" value="flap{NN}"/><part id=… subtype="normal_part"><metadata key="name" …/><metadata key="extruder" value="{slot}"/></part>…</object></config>`). Meshes come from `shape.tessellate(p.tess_tol)`.

- [ ] **Step 1: Failing tests** (build two real flaps, 0 and 57, in a tmp plate)

```python
def test_archive_members(tmp_path):       # the four members exist; XML parses
def test_every_part_has_a_slot(tmp_path): # model_settings: each part extruder in 1..8; body → 1; flap57 red → 3, orange → 4
def test_meshes_are_closed(tmp_path):     # every mesh: >0 triangles, each edge shared by exactly two triangles
def test_items_on_bed(tmp_path):          # translated vertex extents within [0, bed]
def test_plain_mode_has_no_bambu_metadata(tmp_path)
```

- [ ] **Step 2: Run, expect FAIL**
- [ ] **Step 3: Implement** with `zipfile` + `xml.etree.ElementTree`.
- [ ] **Step 4: Run, expect PASS**
- [ ] **Step 5: Commit** — `feat(mechanics): 3mf writer with Bambu per-part AMS slots (#670)`

---

### Task 6: CLI, test plate, README

**Files:** Create `mechanics/flaps/generate.py`, `mechanics/flaps/tests/test_generate.py`; fill `mechanics/flaps/README.md`

**Interfaces:**
- Produces: `main(argv: list[str] | None = None) -> int`; run as `python -m flaps.generate` from `mechanics/` or `python generate.py`.
- Flags: `--test-plate`, `--full` (all plates for one drum: plain plates without a tower, the colour plate with one), `--out DIR` (default `out/`), `--set KEY=VALUE` (repeatable, → `with_overrides`, floats), `--plain-3mf`.
- Test plate contents: flaps whose front shows `% " . , : @ M W 8` and ♥, plus flap 57 (red), plus two variants of `A`: `inlay_depth=0.3` and `thickness=1.2` (built with their own params, named `flapNN_inlay03` / `flapNN_t12`). One plate, with a tower.
- Writes `out/test-plate.3mf` or `out/drum-plate-{k}.3mf` and prints one line per file: path, flap count, colours used.

- [ ] **Step 1: Failing tests**

```python
def test_test_plate_smoke(tmp_path):  # main(["--test-plate","--out",str(tmp_path)]) == 0; test-plate.3mf exists; 13 assemblies
def test_full_set_covers_64(tmp_path):  # --full: union of flap names across plates == flap00..flap63, each once
def test_bad_override_exits_2(tmp_path, capsys):  # --set inlay_depth=0.6 → returns 2, message names the field
```

- [ ] **Step 2: Run, expect FAIL**
- [ ] **Step 3: Implement** `generate.py`; README: venv recipe, commands, AMS slot table, print settings (0.2 mm layers, front face down, textured PEI, prime tower on colour plates), and the tuning loop (`--set cap_frac=…`).
- [ ] **Step 4: Run, expect PASS** — `scripts/check.sh mech` and `scripts/check.sh --touched master` both green.
- [ ] **Step 5: Commit** — `feat(mechanics): flap generator CLI and test plate (#670)`

---

### Task 7: Hand-off (main session)

- [ ] Generate `out/test-plate.3mf`, give Lucas its path; he imports into Bambu Studio and confirms parts arrive on slots 1–8 (fallback: `--plain-3mf` and assign by hand, then file a follow-up issue for the metadata).
- [ ] Post on #670: what was verified and the import result. Close #670 after the import is confirmed; the print result and tuned params go in a comment (and as new defaults in a follow-up commit if they change).
- [ ] Memory: add `mechanics` sync notes (README print recipe, `alphabet.json` is the drum-order source for project 4).
