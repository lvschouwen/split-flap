# KiCad 10 Howto — Bus PCB (Beginner)

**Revision:** 2026-04-27
**Audience:** Absolute KiCad beginner. Spell-out-every-click.
**Time:** 2-4 hours start to Gerbers.
**Companion to:** `KICAD_GETTING_STARTED.md` (read that first),
`SCHEMATIC_BUS.md` (the spec — what we're building),
`LAYOUT_BUS.md` (placement + dimensions).

This howto walks you through making the Bus PCB from absolute scratch
in KiCad 10. It is the simplest of the three boards on purpose — start
here so you build the muscle memory before tackling Unit and Master.

**What you'll end up with:** a 300 × 32 mm DIN-rail bus PCB with two
JST-VH connectors, four long traces (12 V / RS485 A / RS485 B / GND),
and 8 unit-station contact zones. Eight of these boards make a
4-row × 16-unit display.

---

## What this board has

Just two components in the schematic:

- **J_in** — JST-VH 4-pin male, THT (B4P-VH-A, LCSC C144392) — input
  end of the daisy chain.
- **J_out** — same part, output end.

Plus 4 nets (`12V`, `RS485_A`, `RS485_B`, `GND`) and 8 stations of
contact zones drawn directly in the PCB editor as copper polygons.

That's it. The "intelligence" is all in the layout.

---

## Step 0 — Open KiCad and create the project

1. Launch **KiCad** (the icon installed in step 1 of
   `KICAD_GETTING_STARTED.md`).
2. The **Project Manager** opens.
3. **File → New Project**.
4. In the file dialog, navigate to your local copy of the repo →
   `PCB/v2/kicad/`. There may already be `bus_pcb/` and `unit/`
   folders from earlier hand-off attempts — **ignore those** (they're
   stale; see `KICAD_HANDOFF.md` § 2).
5. Click **New Folder** → name it exactly `bus`.
6. Open the new `bus/` folder.
7. **Filename**: `splitflap-bus-v2`. Click **Save**.
8. KiCad asks "Create a new directory for the project?" — uncheck
   that box if it's checked (you already created `bus/`).
9. Click **OK**. KiCad creates `splitflap-bus-v2.kicad_pro`,
   `.kicad_sch`, and `.kicad_pcb` in `bus/`.
10. The Project Manager now shows your new project on the left. The
    schematic and PCB files appear in the file tree.

---

## Step 1 — Open the Schematic Editor

1. In the Project Manager left pane, **double-click
   `splitflap-bus-v2.kicad_sch`**. (Or click the **Schematic Editor**
   button in the top toolbar.)
2. Eeschema opens with a blank page (A4 with a title block).
3. The page is mostly empty grid. The cursor X/Y position shows in
   the bottom status bar.
4. Press **`Home`** to "zoom to fit" — this re-centers your view
   when you get lost.

### Set the page title block (optional but nice)
1. **File → Page Settings**.
2. Fill in:
   - **Title**: `Split-flap Bus PCB v2`
   - **Date**: today
   - **Revision**: `2026-04-27`
   - **Company**: your name or leave blank
3. Click **OK**.

---

## Step 2 — Place the two JST-VH connectors

We need two JST-VH 4-pin male THT connectors.

1. Press **`A`** (Add Symbol). The Symbol Chooser dialog opens.
2. In the search box at the top, type: `JST_VH_B4P-VH-A`.
3. KiCad shows the symbol in the right preview pane: a 4-pin
   connector labeled `J?` with pins 1-4.
4. Double-click the entry (or click → **OK**).
5. Your cursor now carries the symbol. Move it to the left side of
   the page, around grid coordinate `(95, 100)` mm — somewhere
   roughly left-of-center. **Click once to drop it.**
6. The "Add Symbol" tool stays active (multi-place mode). Move to
   the right side around `(170, 100)` and **click again** to place
   the second connector.
7. Press **`Esc`** to exit the placement tool.

You should see two identical 4-pin connectors labeled `J?` on the
schematic.

### Rename them to J_in and J_out
1. Click the **left** connector once to select it (it highlights).
2. Press **`E`** (Edit Properties). The properties dialog opens.
3. Change **Reference** from `J?` to `J_in`. (KiCad will prefix
   with `J` automatically; type `J_in` exactly so the reference
   becomes `J_in`.)
4. Optionally fill the **Value** field with `JST-VH 4P` for
   readability.
5. Click **OK**.
6. Click the **right** connector. Press **`E`**. Reference: `J_out`.
   Click **OK**.

You now have `J_in` (left) and `J_out` (right) on the schematic.

---

## Step 3 — Add net labels for the 4 nets

For each connector, we'll wire a label onto each pin so KiCad knows
which pin is which net. We use **net labels** (not power symbols)
because nothing on this board generates power — all 4 nets just pass
through.

1. Press **`L`** (Add Label).
2. A small dialog opens asking for the label text. Type: `12V`
3. Click **OK**.
4. Your cursor carries a `12V` label. Click on the **wire endpoint of
   J_in pin 1** (the leftmost pin of the left connector — pin 1 is
   marked with a small triangle/dot on the symbol).
5. The label snaps to the pin. The tool stays active.
6. Type the next label and place: `RS485_A` on J_in pin 2.
7. `RS485_B` on J_in pin 3.
8. `GND` on J_in pin 4.
9. Now do J_out: `12V` on pin 1, `RS485_A` on pin 2, `RS485_B`
   on pin 3, `GND` on pin 4.
10. Press **`Esc`** to stop labeling.

**KiCad treats two wires/pins with the same net label as electrically
connected** — so labelling J_in pin 1 = `12V` and J_out pin 1 = `12V`
is what tells KiCad "these are the same net". We don't actually need
to draw wires across the schematic.

> If the label looks like it's not snapped to the pin: click on the
> label, press `M` (Move), and drag it onto the pin. The pin is the
> small line sticking out of the connector body — the label has to
> touch the very end of that line.

---

## Step 4 — Place a "no-connect" only if needed

Skip this step for the Bus board. Both connectors have all 4 pins
in use; nothing should be left floating.

(Save this knowledge for the Master board, where some MCU pins are
intentionally NC.)

---

## Step 5 — Add LCSC custom field

For each connector, add the LCSC part number so the JLC PCBA BOM
will export correctly.

1. Click `J_in`. Press **`E`**.
2. Click **Add Field** at the bottom of the dialog.
3. **Name**: `LCSC` (capital letters, exactly).
4. **Value**: `C144392`.
5. Click **OK** on the field, then **OK** on the properties dialog.
6. Repeat for `J_out`.

(You can also do this in bulk: **Tools → Edit Symbol Fields Table**.
For 2 parts it's faster to do it inline.)

---

## Step 6 — Annotate

KiCad auto-generates designators (`J?` → `J_in`/`J_out`) when you set
them manually, so this step is partly already done — but run
**Annotate** anyway to make sure the schematic is internally
consistent.

1. **Tools → Annotate Schematic**.
2. Leave defaults. Click **Annotate**.
3. The dialog reports something like "0 annotations changed". Close.

---

## Step 7 — Run ERC

1. **Inspect → Electrical Rules Checker**. The dialog opens.
2. Click **Run ERC**.
3. You'll likely get warnings like:
   - **"Input Power pin not driven by any Output Power pin"** — this
     is for `12V` and `GND`. Both nets are passed through; nothing on
     this board generates them. To silence, add `PWR_FLAG` symbols:
     - Press **`A`** → search `PWR_FLAG`. Place a `PWR_FLAG` symbol.
     - Connect it to the `12V` net (just drop it next to the `12V`
       label on J_in pin 1, draw a short wire `W` from PWR_FLAG to
       the pin or label).
     - Place another `PWR_FLAG` on `GND`.
     - Re-run ERC.
4. **Fix every error.** Warnings about pin types are OK if you've
   verified the actual circuit is fine.
5. When ERC is clean, close the dialog.

> If you get "Pin not connected" on a connector pin you're using:
> the net label probably isn't actually touching the pin. Click the
> label → `M` → re-place exactly on the pin endpoint.

---

## Step 8 — Save

Press **`Ctrl+S`**. Always save before opening the PCB Editor.

---

## Step 9 — Open the PCB Editor

1. Switch back to the Project Manager.
2. Double-click **`splitflap-bus-v2.kicad_pcb`**. Pcbnew opens.
3. The board is empty (just a title block). The right side shows
   the **Layer Manager** panel.

### Set Board Setup first (one-time)
1. **File → Board Setup**.
2. Left tree → **Constraints**.
3. Set the JLC-friendly minimums per `KICAD_GETTING_STARTED.md` § 8:
   | Field | Value |
   |---|---|
   | Minimum copper clearance | 0.127 mm |
   | Minimum track width | 0.127 mm |
   | Minimum hole | 0.3 mm |
   | Minimum annular ring | 0.13 mm |
4. Left tree → **Net Classes** → Default.
   - Clearance: 0.15 mm
   - Track width: 0.25 mm
   - Via diameter: 0.6 mm
   - Via drill: 0.3 mm
5. Click **OK**.

---

## Step 10 — Pull components in from the schematic

1. **Tools → Update PCB from Schematic** (`F8`).
2. The "Update PCB" dialog opens.
3. ☑ Re-link footprints to schematic symbols based on their reference
   designators: **OFF** (default — leave off).
4. ☑ Delete footprints with no symbols: **ON**.
5. Click **Update PCB**.
6. KiCad reports something like "ERROR: No footprint assigned to
   symbol J_in" — that's because we haven't told KiCad which footprint
   to use for the JST-VH symbol yet. Close this dialog.

### Assign the footprint
1. Switch back to **Eeschema**.
2. **Tools → Footprint Assignment Tool** (CvPcb-style — opens a 3-pane
   window: symbols on the left, footprint libraries in the middle,
   matching footprints on the right).
3. Click `J_in` in the left pane.
4. In the middle pane, find and click `Connector_JST`.
5. In the right pane, scroll/search for `JST_VH_B4P-VH-A` (the
   footprint of the same name as the symbol).
6. **Double-click** it to assign. The middle column for `J_in` now
   shows `Connector_JST:JST_VH_B4P-VH-A`.
7. Repeat for `J_out` (same library, same footprint).
8. Click **Apply, Save Schematic & Continue**.
9. Close the Footprint Assignment Tool.
10. Save Eeschema (`Ctrl+S`).
11. Switch back to **Pcbnew**. Re-run **Tools → Update PCB from
    Schematic** (F8). This time it should succeed and dump the two
    footprints somewhere on the board area.

---

## Step 11 — Draw the board outline (Edge.Cuts)

We need a **300 mm × 32 mm** rectangle on the **Edge.Cuts** layer.

1. In the Layer Manager (right side), click **Edge.Cuts** to make it
   the active layer (its name highlights, and the layer indicator
   in the toolbar shows "Edge.Cuts").
2. Set the grid to a useful value: **right-click in canvas → Grid →
   Grid: 1.0000 mm** (or use the dropdown in the toolbar).
3. Set drawing origin so coordinates make sense. **Place → Drill and
   Place Offset → Drill/Place Origin** → click somewhere in the
   bottom-left of the page (we'll use this as `(0,0)`).
4. Use the **Add Graphic Line** tool (right toolbar, looks like a
   line with two dots) — or hotkey **`Ctrl+Shift+L`** (depends on
   KiCad 10 build; check your hotkeys with `?`).
5. Draw a rectangle using 4 lines:
   - **Line 1** from `(0, 0)` to `(300, 0)` — top edge.
   - **Line 2** from `(300, 0)` to `(300, 32)` — right edge.
   - **Line 3** from `(300, 32)` to `(0, 32)` — bottom edge.
   - **Line 4** from `(0, 32)` to `(0, 0)` — left edge.
6. Watch the bottom status bar to track cursor position. Click each
   endpoint precisely. Press `Esc` to end the line tool after each
   click pair, or keep clicking to continue from the last endpoint.
7. After drawing, you should see a rectangle outline on the canvas.
   Press **`Home`** to fit-to-window if you can't see it all.

> **Pro tip:** an easier way to draw a perfect rectangle: use **Place
> → Add Rectangle** instead of 4 separate lines. Click one corner,
> then the opposite corner. KiCad 10 has this; it's faster.

---

## Step 12 — Place the connectors

`J_in` and `J_out` are dumped in a default cluster after Update PCB.
Find them (zoom out with `Home` or scroll wheel + `Ctrl`).

1. Click **`J_in`** to select it. Press **`M`** (Move).
2. Watch the bottom status bar X/Y. Move the cursor to **`(10, 16)`
   mm** — left-end, board-vertical-center.
3. Click to drop.
4. If the connector is rotated wrong (pin 1 should face inward, i.e.
   to the right since J_in is on the left edge): press **`R`** to
   rotate 90°. Repeat until pin 1 faces right (toward the board
   stations).
5. Click `J_out`. Press `M`. Move to **`(290, 16)` mm**. Drop.
6. Pin 1 of J_out should face inward (left, toward stations). Press
   `R` if needed.

> **Verifying pin 1 direction:** Pcbnew puts a small triangle or
> rectangle on pin 1 of every footprint, on the silkscreen. If you
> can't see it, zoom in. The schematic spec (`SCHEMATIC_BUS.md` §
> Connector positions) says pin 1 = inward.

---

## Step 13 — Place mounting holes

`LAYOUT_BUS.md` specifies **4** mounting holes along the centerline
(y = 16 mm) at x = 8, 100, 200, 292 mm.

KiCad mounting holes are a special "footprint" with no electrical
connection. Add them like any footprint:

1. Press **`A`** in Pcbnew (Add Footprint).
2. Search: `MountingHole_3.2mm_M3`.
3. Pick the one in the **MountingHole** library (3.2 mm clearance for
   M3 screw, NPTH = no plating).
4. Click to drop, then `M` to position at exactly **(8, 16)**.
5. Repeat for **(100, 16)**, **(200, 16)**, **(292, 16)**.
6. Press `Esc` when done.

You should see 4 small circles on the board centerline. They auto-
assign references like `H1`, `H2`, `H3`, `H4`.

---

## Step 14 — Draw the 4 horizontal traces

This is the main routing job. Each trace runs nearly the full board
length (~280 mm) at a different Y coordinate, with a different width.

| Trace | Y centerline | Width | Layer |
|---|---|---|---|
| 12V | 4 mm  | 5 mm  | F.Cu (top) |
| A   | 12 mm | 1 mm  | F.Cu |
| B   | 20 mm | 1 mm  | F.Cu |
| GND | 28 mm | 5 mm  | F.Cu |

### Make F.Cu the active layer
1. In the Layer Manager (right side), click **F.Cu** to highlight it.

### Draw the 12V trace (5 mm wide, y = 4)
1. Press **`X`** (Route Single Track).
2. Before drawing: in the **track width** box at the top of the
   Pcbnew window (or via **Route → Edit Pre-defined Sizes**), set
   the width to **5 mm** (5000 µm). If it's not in the dropdown,
   click "Edit pre-defined sizes" → add 5 mm → OK.
3. Click on **J_in pin 1** (the connector pin labeled `12V`).
4. Move the cursor right. KiCad shows a "ratsnest" thin line guiding
   you toward J_out pin 1. Move along y = 4 mm — watch the bottom
   status bar — and click at intermediate points to lay segments.
5. End at **J_out pin 1**. Click to finish.
6. Press `Esc` to exit the route tool.

> **Snapping to the right Y:** With a 1 mm grid, your trace will
> auto-snap to multiples of 1 mm. Pin 1 of the connectors is at
> y = 16 (the connector center), but its inner side reaches around
> y = 4 (top pin) due to JST-VH pin spacing. Double-check by clicking
> on the laid trace and looking at its endpoint coordinates in the
> properties panel — adjust if Y drifted.

> **Easier method:** Lay the trace with `X` from pin to pin (KiCad
> handles the route geometry). Then click the trace, press `E`, and
> manually set its width to 5 mm.

### Repeat for the other 3 traces
- Width 1 mm @ y = 12: connect J_in pin 2 → J_out pin 2 (RS485_A).
- Width 1 mm @ y = 20: connect J_in pin 3 → J_out pin 3 (RS485_B).
- Width 5 mm @ y = 28: connect J_in pin 4 → J_out pin 4 (GND).

After all 4 traces, you should see four parallel horizontal lines
on F.Cu spanning most of the board's length.

---

## Step 15 — Draw the 8 contact-zone polygons (per station)

This is the only weird part of the Bus board. We need a widened
copper area at each unit station so the pogo pins have a tolerance
margin to land on. There's no KiCad symbol for these — they're just
copper polygons drawn directly in the PCB editor.

### Stations are at these X coordinates (8 per board, 37 mm pitch)
| Station | x (center) |
|---|---|
| 0 | 18  |
| 1 | 55  |
| 2 | 92  |
| 3 | 129 |
| 4 | 166 |
| 5 | 203 |
| 6 | 240 |
| 7 | 277 |

### At each station, 4 zones (one per net)
| Net | Y center | Shape | Connected to |
|---|---|---|---|
| 12V | 4  | 5 × 5 mm rectangle | 12V trace |
| A   | 12 | 3 × 5 mm rectangle | RS485_A trace |
| B   | 20 | 3 × 5 mm rectangle | RS485_B trace |
| GND | 28 | 5 × 5 mm rectangle | GND trace |

### How to draw one (12V at station 0 = (18, 4))
1. Make sure **F.Cu** is the active layer.
2. **Place → Add Filled Zone** (or right-click canvas → Add Filled
   Zone). The zone properties dialog opens.
3. **Layer**: F.Cu.
4. **Net**: select `12V` from the dropdown.
5. **Zone properties**: keep defaults (filled, no thermal relief
   needed since we want a solid contact).
6. Click **OK**.
7. Now your cursor draws polygon vertices. We want a 5 × 5 mm rect
   centered at (18, 4):
   - Click **(15.5, 1.5)** — top-left corner.
   - Click **(20.5, 1.5)** — top-right.
   - Click **(20.5, 6.5)** — bottom-right.
   - Click **(15.5, 6.5)** — bottom-left.
   - Press `Enter` (or right-click → Close zone) to finish the polygon.
8. Press **`B`** to fill all zones — the rectangle should now be
   solid copper, joined to the 12V trace passing through y = 4.

### Repeat for all 8 stations × 4 nets = 32 zones

This is tedious but mechanical. Tip: draw all 4 zones at station 0,
then **select them all** (drag a selection box around the 4 polygons),
**Edit → Copy** (`Ctrl+C`), **Paste** (`Ctrl+V`), and place at
station 1's offset (37 mm to the right). Repeat 6 more times.

After all 32 zones are placed and `B` filled them, you should see a
clean copper "ladder" pattern on F.Cu: 4 long traces with 8 widened
landing areas evenly spaced.

> **Faster alternative:** instead of 32 separate filled zones, you
> can simply **widen each trace at the station** — set the trace
> width temporarily to 5 mm (or 3 mm for A/B) and route a short
> segment over the station. This is electrically identical. The
> dedicated zones are slightly easier to mask-clear in step 16.

---

## Step 16 — Open the solder mask over each contact zone

The contact zones need to be **bare ENIG copper** (no green mask) so
the pogo pin tip can press directly on the gold-plated copper.

For each of the 32 contact zones, draw a corresponding rectangle on
the **F.Mask** layer of identical dimensions (slightly larger if you
want a bit of mask pull-back margin — 0.1 mm on each side is fine).

1. In the Layer Manager, click **F.Mask** to make it active.
2. **Place → Add Filled Zone** (or use a graphic rectangle).
3. **Layer**: F.Mask.
4. **Net**: leave as `<no net>` — mask layers don't need nets.
5. Draw a rectangle of the **same dimensions** as the underlying F.Cu
   contact zone (or slightly larger by 0.1 mm).
6. Repeat for every contact zone.

> **Faster bulk method:** select all 32 F.Cu contact zones, copy them
> (`Ctrl+C`), paste, then change the layer of the pasted set to
> F.Mask via **Edit Properties → Layer**. This duplicates the geometry
> exactly.

The F.Mask layer is **negative** in KiCad — anything drawn on F.Mask
means "no mask here, leave bare copper exposed". So drawing
rectangles on F.Mask over the contact zones is correct.

---

## Step 17 — Add the bottom GND zone

The bottom layer is a continuous GND pour. This gives the
differential A/B pair on top a clean reference.

1. In the Layer Manager, click **B.Cu** to make it active.
2. **Place → Add Filled Zone**.
3. **Layer**: B.Cu.
4. **Net**: `GND`.
5. Click **OK**.
6. Draw a polygon following the board outline minus 1 mm clearance
   to the edge:
   - Top-left: `(1, 1)`
   - Top-right: `(299, 1)`
   - Bottom-right: `(299, 31)`
   - Bottom-left: `(1, 31)`
   - `Enter` to close.
7. Press **`B`** to fill. The whole bottom layer (minus 1 mm border)
   should now be solid copper labeled GND.

---

## Step 18 — Add the via fence (top GND ↔ bottom GND)

Stitch vias around the perimeter every ~10 mm. This connects the top
GND trace (y = 28) to the bottom GND pour, and provides return-current
continuity for the A/B pair.

1. Press **`V`** (Add Via) to place a single via at the cursor. Track
   width and via size come from the Net Class default (0.6 mm
   diameter, 0.3 mm drill).
2. Place vias along the bottom edge (between the GND trace and the
   board edge), spaced ~10 mm apart, all on the GND net (KiCad will
   auto-detect the net if you place the via on the GND trace).
3. Optionally place a similar fence along the top edge (between the
   12V trace and the top board edge) — but the 12V trace is
   functionally a power rail, not a return, so this is less critical.
4. Press **`Esc`** when done.

> Skipping the via fence is OK for a 250 kbaud RS-485 bus over 600 mm.
> EMC-tighter implementations would do it; we won't fail because we
> didn't.

---

## Step 19 — Add silkscreen labels

The silkscreen helps assembly + debugging. Required labels per
`LAYOUT_BUS.md`:

- "0" through "7" near each station (above or below the station,
  doesn't matter).
- "TOP" above the 12V trace (somewhere near the top edge).
- "BOTTOM" below the GND trace.
- Pin 1 markers next to J_in and J_out (KiCad's footprint already
  marks pin 1 with a small dot/triangle on silk).
- "v2" + revision date somewhere — bottom-left corner is fine.

To add text:

1. Click **F.Silkscreen** in the Layer Manager.
2. Press **`T`** (Add Text). Or **Place → Add Text**.
3. Type the text. Set size: 1.5 mm height, 0.2 mm thickness is
   readable. Set layer: F.Silkscreen.
4. Click to drop. Press `Esc` when done.

For station numbers, place each `0` through `7` directly above the
station's contact zones (around y = 1, x matching each station).

---

## Step 20 — Run DRC

1. Press **`B`** one more time to make sure all zones are filled
   with the latest geometry.
2. **Inspect → Design Rules Checker**. Click **Run DRC**.
3. Fix every error. Common ones for the Bus board:
   - **Unconnected items**: maybe a station's contact zone is not
     touching its trace. Click the marker → highlight → either
     extend the trace into the zone or move the zone.
   - **Copper too close to edge**: a contact zone or trace is
     within 0.5 mm of the board edge. Move the offending item or
     reduce its size.
   - **Silk over solder mask opening**: text overlapping a contact
     zone. Move the text away from contact zones.
4. Once DRC is clean, save (`Ctrl+S`).

---

## Step 21 — 3D viewer sanity check

1. **View → 3D Viewer** (or **`Alt+3`**).
2. Wait for the render. Rotate with right-mouse drag.
3. Look for:
   - The two JST-VH connectors at the correct ends.
   - 4 mounting holes along the centerline (visible as cylindrical
     drilled holes).
   - 8 stations of contact zones visible on top — they should
     appear as bare copper (gold-ish, not green) because we
     removed mask there.
   - Bottom GND pour as a uniform copper plane.
4. Close 3D viewer.

If anything looks wrong (connector backwards, contact zones missing,
mask not opened over a zone), go fix it in 2D and re-render.

---

## Step 22 — Plot Gerbers

Follow `KICAD_GETTING_STARTED.md` § 9 verbatim. Quick recap:

1. **File → Plot**.
2. Output directory: `gerbers/` (relative to `bus/` project folder).
3. Layers to plot:
   - F.Cu, B.Cu
   - F.Mask, B.Mask
   - F.Silkscreen, B.Silkscreen
   - Edge.Cuts
   - (skip F.Paste / B.Paste — no SMT components on this board)
4. ☑ Use Protel filename extensions: ON.
5. Click **Plot**.
6. Click **Generate Drill Files**:
   - mm units, Excellon format, decimal 4:6 precision.
   - PTH and NPTH separate files (default).
7. Close.

Inspect the `gerbers/` folder. You should see:
- `splitflap-bus-v2-F_Cu.gtl`
- `splitflap-bus-v2-B_Cu.gbl`
- `splitflap-bus-v2-F_Mask.gts`
- `splitflap-bus-v2-B_Mask.gbs`
- `splitflap-bus-v2-F_Silkscreen.gto`
- `splitflap-bus-v2-B_Silkscreen.gbo`
- `splitflap-bus-v2-Edge_Cuts.gm1`
- `splitflap-bus-v2-PTH.drl`
- `splitflap-bus-v2-NPTH.drl`

Zip the entire folder. Upload to JLCPCB.

---

## Step 23 — JLCPCB order checklist

When uploading to JLC, verify:
- **PCB dimensions**: 300 × 32 mm (JLC's preview shows this).
- **Layers**: 2.
- **Material**: FR-4.
- **Thickness**: 1.6 mm.
- **PCB color**: green (or anything — solder mask color).
- **Surface finish**: **ENIG (gold)** — **NOT HASL.** Pogo pins on
  the unit underside need a hard, reliable contact surface; ENIG
  gives that. HASL wears out under repeated mating.
- **Copper weight**: 1 oz both sides.
- **Quantity**: 10 (covers 4 rows × 2 boards/row + 2 spares).

JLC's price for ENIG at this size, qty 10, is roughly **EUR 50-70
total** (~EUR 5-7/board). HASL would be ~half that — pay the premium.

---

## Verification before clicking "Place Order"

Open `LAYOUT_BUS.md` § "Verification before Gerber" and tick every
item against your KiCad design. The critical ones for this board:

- [ ] Outline 300 × 32 mm.
- [ ] **4** mounting holes (not 2) at (8, 16), (100, 16), (200, 16),
      (292, 16).
- [ ] 4 traces at correct Y positions: 4 / 12 / 20 / 28 mm.
- [ ] 8 stations at 37 mm pitch starting at x = 18 mm.
- [ ] Each station has 4 contact zones of correct dimensions.
- [ ] **No 5th PG_KEY pad anywhere.** This is an anti-pattern from
      an earlier draft. The unit's DIN clip provides polarization,
      not a 5th pogo.
- [ ] Solder mask removed over every contact zone (F.Mask rectangles
      drawn).
- [ ] Both connectors are JST-VH 4-pin (B4P-VH-A, C144392), NOT
      JST-XH.
- [ ] Pin 1 of J_in and J_out maps to 12V.
- [ ] **ENIG** plating selected in JLC's order form (NOT HASL).
- [ ] DRC passes.

---

## What you learned

By finishing this board, you've used:
- Schematic editor (Eeschema): symbol placement, net labels, ERC.
- Footprint Assignment Tool.
- PCB editor (Pcbnew): board outline, footprint placement, mounting
  holes, single-track routing, custom-width traces, filled zones,
  mask openings, silkscreen text, vias, DRC, Gerber export.

This is **most of what KiCad does**. The Unit and Master howtos
build on this foundation — they have more components and more nets,
but the workflow is identical.

**Next:** open `KICAD_HOWTO_UNIT.md`.
