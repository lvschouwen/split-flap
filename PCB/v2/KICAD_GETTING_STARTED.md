# KiCad 10 — Getting Started (Beginner)

**Revision:** 2026-04-27
**Audience:** You have never opened KiCad before. You will lay out the
3 split-flap v2 boards yourself.
**Companion to:** `KICAD_HOWTO_BUS.md` → `KICAD_HOWTO_UNIT.md` →
`KICAD_HOWTO_MASTER.md` (do them in that order — Bus is simplest).
**Reference (don't follow blindly):** `KICAD_HANDOFF.md` is the terse
spec/decision log. This doc is the click-by-click walkthrough.

This doc covers everything that's the **same for all 3 boards** so the
per-board howtos can stay focused. Read this once, then keep it open
in another tab while you work.

---

## 0. What KiCad is, in one paragraph

KiCad is two main editors that share a project:

1. **Schematic Editor (Eeschema)** — you draw symbols (resistor, MCU,
   connector) and wires between their pins. Output: a netlist
   (which pin connects to which).
2. **PCB Editor (Pcbnew)** — you place footprints (the physical
   land patterns of those symbols) on a board, route copper traces
   between the pads, and export Gerbers (the files JLCPCB needs).

You always go schematic → PCB, never the other way. Every component
in the PCB came from the schematic via "Update PCB from Schematic".

---

## 1. Install KiCad 10

1. Go to **https://kicad.org/download/**.
2. Pick your OS (Linux/macOS/Windows). Download the **stable 10.x**
   release. Do NOT pick "nightly" — it's a moving target.
3. Run the installer. Accept defaults. **Make sure the "default
   libraries" checkbox is ticked** — those cover ~90% of what we need.
4. Launch KiCad. You should see the **Project Manager** window with
   buttons for "Schematic Editor", "Symbol Editor", "PCB Editor",
   "Footprint Editor", "Gerber Viewer", "Image Converter",
   "Calculator Tools", "Drawing Sheet Editor", "Plugin and Content
   Manager".

If the launcher won't open or any of those buttons are missing, the
install didn't pick up the libraries. Re-run the installer with the
"Reset to defaults" option.

---

## 2. Folder layout for this project

We'll keep the 3 KiCad projects under `PCB/v2/kicad/`:

```
PCB/v2/kicad/
├── master/   (you'll create this in KICAD_HOWTO_MASTER.md)
├── unit/     (KICAD_HOWTO_UNIT.md)
└── bus/      (KICAD_HOWTO_BUS.md — start here)
```

A KiCad "project" is a folder containing 3 files with the same stem:
`<name>.kicad_pro` (settings), `<name>.kicad_sch` (schematic),
`<name>.kicad_pcb` (PCB). Plus auto-generated junk like `*-backups/`.

**Don't open the existing `kicad/unit/unit.net` or
`kicad/bus_pcb/bus_pcb.net` files** — those are stale netlist exports
from a pre-merge state with the wrong USART pinout and the wrong LDO.
You'll start each project fresh from the `SCHEMATIC_*.md` ground truth.

---

## 3. KiCad UI orientation (read this once)

### Project Manager window
- Left pane: file tree of the open project.
- Top buttons: launch each editor. Double-clicking a `.kicad_sch`
  file opens Eeschema; double-clicking `.kicad_pcb` opens Pcbnew.

### Eeschema (Schematic Editor) — what's where
- **Top toolbar**: undo/redo, zoom to fit (`Home` key), Plot,
  Annotate, ERC.
- **Right toolbar (vertical)**: place tools. The two you'll use
  constantly:
  - **A** (Add Symbol) — opens the symbol picker.
  - **W** (Add Wire) — draws a wire. Press `Esc` to stop wiring.
  - **L** (Add Label) — local net label.
  - **P** (Add Power Symbol) — `GND`, `+3V3`, `+12V`, etc.
  - Pencil + grid icon (Add → Junction, hotkey `J`) — explicit
    dot at a 4-way wire intersection. KiCad usually auto-junctions,
    but verify for T-intersections.
  - **Q** (No Connect flag, hotkey `Q`) — small `X` you put on
    pins you intentionally leave unconnected (silences ERC).
- **Bottom status bar**: cursor X/Y in mm, current grid spacing,
  current zoom %.

### Pcbnew (PCB Editor) — what's where
- **Top toolbar**: Plot (Gerber export), DRC, fill all zones (`B`).
- **Right toolbar (vertical)**:
  - **X** — Route single track.
  - **6** — Route differential pair (KiCad 10 native).
  - Filled-zone tool (paint icon).
  - Add graphic line (for Edge.Cuts board outline).
  - Add dimension, add text, add via.
- **Layer manager (right side panel)**: shows all the copper +
  silkscreen + mask layers. Click a layer name to make it the
  *active* layer (new traces go on it). The colored swatch toggles
  visibility.
- **Bottom**: cursor X/Y, current grid, layer status.

### Universal hotkeys (memorize these 8)
| Key | Action |
|---|---|
| `M` | Move (hover an item, press `M`, click to place) |
| `R` | Rotate selected item 90° |
| `F` | Flip to other side (PCB only — flips component to back) |
| `E` | Edit properties of selected item |
| `Del` | Delete selected |
| `Ctrl+Z` | Undo |
| `Esc` | Cancel current tool |
| `?` | Show all hotkeys for the current editor |

If you forget one: **Help → List Hotkeys** (or just press `?`).

---

## 4. Setting up libraries

KiCad 10's built-in symbol + footprint libraries cover most of our
parts. The exceptions (custom symbols/footprints we draw ourselves)
live in **a per-board custom library** that we'll create when the
howto tells you to.

### Mapping our parts to KiCad libraries

This table tells you which KiCad library symbol + footprint to pick
for each part in our BOMs. **`KICAD_HANDOFF.md` § 3** has the full
table — bookmark that page. The most-used entries:

| Our part | Symbol library | Footprint library |
|---|---|---|
| Resistor (any value) | `Device:R` | `Resistor_SMD:R_0603_1608Metric` |
| Capacitor (any value) | `Device:C` | `Capacitor_SMD:C_0603_1608Metric` (etc — see BOM column) |
| LED | `Device:LED` | `LED_SMD:LED_0805_2012Metric` |
| Generic diode | `Device:D` | per BOM |
| 10 V Zener (BZT52C10) | `Diode:BZT52C` (or `Device:D_Zener`) | `Diode_SMD:D_SOD-123` |
| TVS SMAJ15A | `Device:D_TVS` (or `Diode_TVS:SMAJ15A-13-F` if present) | `Diode_SMD:D_SMA` |
| ESD SM712-02HTG | (custom — hand-draw 3-pin) | `Package_TO_SOT_SMD:SOT-23` |
| AO3401A P-FET | `Transistor_FET:AO3401A` (or generic P-channel) | `Package_TO_SOT_SMD:SOT-23` |
| AOD409 P-FET | `Transistor_FET` (generic P-channel, DPAK) | `Package_TO_SOT_SMD:TO-252-2` |
| LDL1117S33 LDO | `Regulator_Linear:LM1117-3.3` (LM1117 family pinout) | `Package_TO_SOT_SMD:SOT-223-3_TabPin2` |
| K7803-1000R3 buck | (custom — see Master howto) | `Package_TO_SOT_THT:TO-220-3_Vertical` (or any L78xx SIP-3 — verify pin order) |
| TPL7407L stepper drv | `Driver_Motor:ULN2003A` (footprint-compatible) | `Package_SO:SOIC-16_3.9x9.9mm_P1.27mm` (**narrow!**) |
| STM32G030K6T6 | `MCU_ST_STM32G0:STM32G030K6Tx` | `Package_QFP:LQFP-32_7x7mm_P0.8mm` |
| ESP32-S3-WROOM-1-N16R8 | `RF_Module:ESP32-S3-WROOM-1` | `RF_Module:ESP32-S3-WROOM-1` |
| SC16IS740IPW UART expander | (custom — hand-draw) | `Package_SO:TSSOP-16_4.4x5mm_P0.65mm` |
| SN65HVD75DR RS-485 PHY | `Interface_UART:SN65HVD75D` (or generic 8-pin) | `Package_SO:SOIC-8_3.9x4.9mm_P1.27mm` |
| USBLC6-2SC6 USB ESD | `Power_Protection:USBLC6-2SC6` | `Package_TO_SOT_SMD:SOT-23-6` |
| 14.7456 MHz crystal | `Device:Crystal` | `Crystal:Crystal_SMD_3225-4Pin_3.2x2.5mm` |
| USB-C 16-pin SMD | `Connector_USB:USB_C_Receptacle_USB2.0_16P` | `Connector_USB:USB_C_Receptacle_GCT_USB4085` |
| JST-VH 4-pin male THT | `Connector_JST:JST_VH_B4P-VH-A` | `Connector_JST:JST_VH_B4P-VH-A` |
| JST-XH 5-pin male THT | `Connector_JST:JST_XH_B5B-XH-A` | `Connector_JST:JST_XH_B5B-XH-A` |
| JST-XH 3-pin male THT | `Connector_JST:JST_XH_B3B-XH-A` | `Connector_JST:JST_XH_B3B-XH-A` |
| KF7.62 2-pin screw terminal | `Connector:Screw_Terminal_01x02` (generic) | `TerminalBlock_4Ucon:TerminalBlock_4Ucon_1x02_P3.50mm_Horizontal` (or any 7.62 mm pitch THT footprint matching the BOM part) |
| Tact switch 6×6 SMD | `Switch:SW_Push` | `Button_Switch_SMD:SW_SPST_B3U-1000P` (or any 6×6 SMD tact) |
| Mounting hole M3 | (none — placed in PCB editor) | `MountingHole:MountingHole_3.2mm_M3` |
| Mill-Max pogo 0906-2 | (custom — see Bus howto) | (custom — single hole 1.83 mm drill, 2.45 mm pad) |

When you can't find a part: **File → Symbol Editor → File → New
Symbol** to draw your own. Same for footprints in the Footprint
Editor. The Bus howto walks you through creating the custom pogo-pin
footprint as a worked example — once you've done that, drawing the
SC16IS740 / K7803 / SM712 symbols follows the same pattern (just
rectangles + named pins).

### LCSC field for JLC PCBA

If you're going to use JLC's assembly service, every part needs an
**`LCSC`** custom field on its symbol. The value is the LCSC part
number from the BOM CSV (e.g., `C113973`).

**Bulk way (recommended):** in Eeschema, **Tools → Edit Symbol Fields
Table**. KiCad opens a spreadsheet of every symbol; add a column
`LCSC`, then paste-fill from the BOM CSV. Save.

**One-by-one way:** click a symbol → press `E` (Edit Properties) →
"Add Field" → Name: `LCSC`, Value: e.g. `C113973`. OK.

You only need `LCSC` if you're using JLC PCBA. For just bare PCBs,
skip it — but we strongly recommend filling it once anyway so the
exported BOM is self-describing.

---

## 5. The schematic-to-PCB flow (mental model)

For every board, the workflow is the same:

```
  1. New project              (File → New → Project)
  2. Open Schematic Editor    (double-click .kicad_sch)
  3. Place all symbols        (A key)
  4. Wire them up             (W key)
  5. Place power symbols      (P key)
  6. Add net labels           (L key)
  7. Annotate                 (Tools → Annotate Schematic)
  8. Run ERC                  (Inspect → Electrical Rules Checker)
  9. Open PCB Editor          (sidebar button or double-click .kicad_pcb)
 10. Update PCB from Schema   (Tools → Update PCB from Schematic — F8)
 11. Set up Board Setup       (File → Board Setup) — DRC rules, layers
 12. Draw board outline       (on Edge.Cuts layer)
 13. Place footprints         (drag from the cluster KiCad spat out)
 14. Route traces             (X for single, 6 for diff-pair)
 15. Add filled zones         (right-click → Add Filled Zone)
 16. Press B to fill          (KiCad doesn't auto-fill)
 17. Run DRC                  (Inspect → Design Rules Checker)
 18. 3D viewer sanity check   (Alt+3 or View → 3D Viewer)
 19. Plot Gerbers             (File → Plot)
 20. Generate drill files     (File → Plot → Generate Drill Files)
 21. Zip + upload to JLC
```

Every per-board howto walks you through these steps in order. After
Bus, the structure will feel familiar — Unit and Master only differ
in **what** you place and route, not **how**.

---

## 6. ERC vs DRC (what they actually check)

These are KiCad's two automated checkers. Different tools, different
problems.

**ERC = Electrical Rules Checker** (schematic only)
- Catches: unconnected pins, two outputs driving the same net, power
  pins not driven by a power source, missing power ground, etc.
- Run from Eeschema: **Inspect → Electrical Rules Checker**.
- **You must fix all errors before moving to PCB.** Warnings are
  often "this pin has no power flag" — usually fine for module pins
  marked as "passive". Read each one; don't bulk-dismiss.
- Common false positives:
  - "Input Power pin not driven" on a module like ESP32-S3 — add a
    `PWR_FLAG` symbol on the +3V3 net to tell KiCad "yes, something
    drives this". Same for `GND`.
  - "Pin not connected" on a chip pin you intentionally leave
    floating — drop a No-Connect flag on it (`Q` key).

**DRC = Design Rules Checker** (PCB only)
- Catches: traces too thin, copper too close to copper, drill too
  small, traces crossing the board edge, unrouted nets, footprint
  collisions, etc.
- Run from Pcbnew: **Inspect → Design Rules Checker**.
- **You must fix all errors before exporting Gerbers.** "Unconnected
  items" warnings are sometimes intentional (e.g., test pads not
  routed), but usually mean you forgot to route something.

Both checkers use the rules you set in **File → Board Setup**. We'll
set those once per board (same values for all 3 — see § 8 below).

---

## 7. Custom fields on parts

Add custom fields in the symbol's properties (`E` on a selected
symbol). The fields we use:

| Field name | Value | Used by |
|---|---|---|
| `LCSC` | LCSC part number from BOM CSV | JLC PCBA BOM export |
| `MPN` | Manufacturer part number | Documentation |
| `Datasheet` | URL | KiCad's right-click "Open Datasheet" |

The value/reference fields (`Value`, `Reference`) are filled
automatically when you place a symbol — `Reference` is the
designator (R1, C12, U3 — auto-incremented when you Annotate).

---

## 8. Standard Board Setup (use on all 3 boards)

After "Update PCB from Schematic" the first time, open **File →
Board Setup**. Set these values — same for all 3 boards.

### Page → Page Settings
- Page size: **A4 landscape** (paper size for the title block).

### Board Stackup
- 2-layer copper, 1.6 mm FR-4. (KiCad's default is fine.)

### Net Classes (we use one per board for now: "Default")
| Param | Default value |
|---|---|
| Clearance | 0.15 mm |
| Track width | 0.25 mm (signal default) |
| Via diameter | 0.6 mm |
| Via drill | 0.3 mm |

For wider traces (12 V power), you set the width per-trace as you
draw. We don't bother with separate net classes for the v2 boards.

### Constraints (the JLC-friendly minimums)
| Param | Value |
|---|---|
| Minimum copper clearance | 0.127 mm (5 mil) |
| Minimum track width | 0.127 mm (5 mil) |
| Minimum drill | 0.3 mm |
| Minimum annular ring | 0.13 mm |
| Edge clearance to copper | 0.5 mm |
| Edge clearance to silkscreen | 0.3 mm |

These are JLC's standard rules. Setting them in DRC means KiCad
warns you before you ship something the fab can't make.

### Solder Mask / Paste
- Mask expansion: 0.05 mm (default).
- Paste expansion: 0 mm (default).

You don't need to touch these.

---

## 9. Plot Gerbers + drill files (every board)

Same procedure on all 3 boards:

1. **Inspect → Design Rules Checker** — fix all errors first.
2. **Press `B`** in Pcbnew — refills all copper zones (KiCad does
   NOT auto-refill on save).
3. **File → Plot**.
4. **Output directory**: `gerbers/` (relative to the project folder
   — KiCad will create it).
5. **Plot format**: Gerber.
6. **Layers to include**:
   - `F.Cu` (top copper)
   - `B.Cu` (bottom copper)
   - `F.Mask` (top solder mask)
   - `B.Mask` (bottom solder mask)
   - `F.Silkscreen` (top silk)
   - `B.Silkscreen` (bottom silk)
   - `F.Paste` (top stencil — only if doing JLC PCBA)
   - `B.Paste` (bottom stencil — only if doing JLC PCBA on bottom)
   - `Edge.Cuts` (board outline)
7. **Options**:
   - ☑ Plot border and title block: OFF
   - ☑ Plot footprint values: OFF
   - ☑ Plot footprint references: ON
   - ☑ Use Protel filename extensions: ON  ← **important for JLC**
   - ☑ Subtract solder mask from silkscreen: ON
   - ☑ Coordinate format: 4.6, unit mm
8. Click **Plot**.
9. Click **Generate Drill Files**:
   - Drill units: mm
   - Format: Excellon
   - ☑ Mirror Y axis: OFF
   - ☑ Minimal header: OFF
   - Drill origin: Absolute
   - PTH and NPTH: separate files (default)
   - Map file: PDF (helpful for sanity checking)
10. Click **Generate Drill File**.
11. Close the Plot dialog.
12. **Zip the entire `gerbers/` folder.** Upload that zip to JLCPCB.

JLC's web upload will preview the board. Sanity-check that:
- The outline is correct dimensions.
- All your traces show up on the right side (top copper red,
  bottom copper blue in JLC's preview).
- Drill holes are where you placed them.
- The silkscreen text is the right size and not under any
  components.

---

## 10. JLC PCBA (assembly service) — optional

If you want JLC to populate parts (PCBA), you also need:

### BOM export
- **Tools → Generate BOM** (or **File → Fabrication Outputs → BOM**).
- Use the "JLCPCB" template if KiCad 10 ships one; otherwise CSV
  with columns: `Comment` (Value), `Designator` (Reference),
  `Footprint`, `LCSC` (the custom field you added in § 4).

### CPL (centroid / pick-and-place) export
- **File → Fabrication Outputs → Component Placement (.pos) Files**.
- Units: mm.
- Format: ASCII, comma-separated.
- ☑ Use auxiliary axis as origin (set the aux origin to the
  bottom-left corner of your board first via **Place → Drill and
  Place Offset → Drill/Place Origin**).
- Columns: Designator / Mid X / Mid Y / Layer / Rotation.

### Easier: install the **Fabrication Toolkit** plugin
- KiCad's **Plugin and Content Manager** (Project Manager → top
  menu) → search **"Fabrication Toolkit"** → Install.
- After install, a new toolbar button shows up in Pcbnew. One
  click exports Gerbers + Drill + BOM + CPL all in JLC's expected
  format. Highly recommended.

---

## 11. 3D viewer (catch placement collisions visually)

In Pcbnew: **View → 3D Viewer** (or `Alt+3`).

This is the single best sanity check before ordering. It renders
your board with all the 3D models KiCad has for the footprints
you used. Things you'll catch:
- A capacitor placed on top of a connector.
- A through-hole pin hitting a trace on the bottom layer.
- A mounting hole eating into a copper pour with no clearance.
- The board outline being wrong (cut into the components).

Rotate with right-mouse drag. Zoom with the scroll wheel. Layer
visibility toggles in the side panel.

If a footprint has no 3D model, it just shows up as a flat colored
shape. For our custom parts (pogo pins, K7803), that's fine —
we're verifying placement, not aesthetics.

---

## 12. When something goes wrong

### Eeschema won't let me wire to a pin
- The wire endpoint must be exactly on the pin's *grid intersection*.
  Press `Ctrl+M` to "find marker" / `Esc` to abort, then check the
  grid is set to the same value as the symbol library (default
  50 mil = 1.27 mm).

### "Update PCB from Schematic" deleted my routing
- It shouldn't unless you ticked "Re-link footprints based on their
  reference designators". The default is "Update existing footprints"
  which preserves placement and routing. Always read the dialog
  options before clicking OK.

### DRC complains about something I can't see
- Click the marker in the DRC dialog → "Highlight" → it zooms to
  the offender. Common culprits: silkscreen text overlapping a pad
  edge (move the text), a 12 V trace too close to a 3.3 V trace
  (widen clearance or move).

### My Gerber preview on JLC's site looks wrong
- 9 times out of 10: you forgot to press `B` to refill zones before
  plotting.
- Otherwise: did you include `Edge.Cuts` in the plot layers? Without
  it, JLC has no idea what shape to cut.

### KiCad crashes
- Save often (`Ctrl+S`). KiCad's auto-recovery is OK but not great.
- The `*-backups/` folder beside your project files contains zipped
  backups — open the most recent one if a session was lost.

---

## 13. Checklist: am I ready to start a board howto?

- [ ] KiCad 10 launches and shows the Project Manager.
- [ ] You found `Help → List Hotkeys` and verified `A` opens the
      symbol picker in Eeschema.
- [ ] You found the **Plot** dialog under File in Pcbnew.
- [ ] You opened **File → Board Setup** at least once and saw the
      "Constraints" page.
- [ ] You bookmarked `KICAD_HANDOFF.md` § 3 (the part-to-library
      mapping table).

If all of those are true, open `KICAD_HOWTO_BUS.md` and start there.
Bus is the simplest of the three boards — by the time you finish it,
the rest of the workflow will feel familiar.
