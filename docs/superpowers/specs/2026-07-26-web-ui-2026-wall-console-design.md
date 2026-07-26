# Web UI 2026 — the Wall Console (#396)

**Status:** approved design, not yet implemented
**Scope:** the Master web UI (`firmware/v2/Master/data/`), plus one firmware
prerequisite (#403). FollowerEsp01 serves no HTML and is untouched.
**Supersedes:** `2026-07-05-web-ui-restructure-design.md`,
`2026-07-08-web-ui-overhaul-design.md`, `2026-07-12-web-ui-visibility-design.md`.
The three-tab information architecture those specs established and extended is
discarded, not amended.
**Reference artefact:** `2026-07-26-web-ui-2026-wall-console-mockup.html` beside
this file — the static mockup that cleared the phone gate. Its device readings
are a snapshot taken while designing, not live data.

## Problem

The shipped UI scores 13/30 against the Rams principles. Two of those scores
are the whole story: **#10 "as little design as possible" = 0** and
**#9 "environmentally friendly" = 3**. It is disciplined about weight and
undisciplined about everything else.

The cause is not bad taste, it is an unrevisited decision. The IA was chosen
once — three tabs, 2026-07-05 — and then #245, #251, #294, #318, #329 and #289
each correctly added a card to it. No single addition was wrong. The result is
5 tabs, 19 cards, 66 static controls, and Maintenance alone carrying 7 cards.

Measured symptoms, which double as the yardstick for #398:

- 5 status-message implementations, 3 upload-with-progress implementations,
  2 reflash pollers, 2 banner mechanisms, 18 bespoke `confirm()` sites against
  1 reusable helper, 7 dead element IDs.
- 29 flexbox rules against 1 grid; one 560 px breakpoint; a 900 px centred
  column. There is no layout system above the card.
- Zero `aria-live` regions on a UI that is entirely live state. 1 landmark.
- `--amber` declared twice (`#FFB000` dead, `#D9A03F` wins), so the shipped
  accent is not the approved one. `.btn.danger` is 4.26:1, under AA, on every
  destructive button.
- Two label→behaviour mismatches: *Stop & blank display* silently cancels an
  in-flight reflash; *Refresh (re-polls every unit)* never passes `probe=1`.

A fourth restyle would repeat the first three. The content layer was never
designed — it accreted — so this is a ground-up rebuild of the default path.

## The stance

**The page is the wall, not an admin panel with a preview on it.**

This is the load-bearing choice, and it is what separates this design from the
options rejected during the design rounds. Those were all navigation
mechanisms: split the audience across two routes, collapse the cards into an
accordion, rebalance the tabs, gate the rare operations behind a service mode.
Every one reorganises the admin panel. None of them stops it being an admin
panel.

**The test for any future proposal: does it change the product's stance, or
only its doors?**

## The model — two object types, not one

A rows-only model recreates the current dishonesty with nicer geometry. The
wall is made of two different kinds of thing:

- **Rows** — flaps mounted on the wall.
- **Controllers** — boxes running firmware.

A controller may own a row (`.91` owns row 1, sixteen units), may own a row
with its own firmware lineage (`.121` owns row 0, five units, ESP-01), or may
own none at all (`.20` is a headless spare). **A controller with no row renders
nothing on the board and says so in its own words on its own page.** It never
appears as an empty row, because an empty row is a lie about the hardware.

## The three screens

Three object levels: **wall → controllers → one controller** (→ one unit,
deliberately not designed yet, see Open questions).

### Home is the wall

Home carries exactly three things:

1. **The board**, at true physical proportion, with the live text on it.
2. **A source line** naming what put that text there and when.
3. **The composer** — one field, *Send to wall*, *Blank it* — plus a plain
   statement of capacity ("21 characters fit on this wall — 5 on row 0,
   16 on row 1").
4. **One line for hardware**: `Controllers · 3 · all well`.

Nothing else. Everything the old home page carried lives on the object it
belongs to.

### The wall is the navigation

**Tapping a row on the board opens the controller that drives it.** The
physical object is the menu. There is no settings tab to find your way back out
of. The hardware line is the only other door, and it leads to the controller
list for the boxes that have no row to tap.

### The controller page is ordered physically

Top to bottom, the order walks the hardware:

1. **The box** — firmware rev, uptime with the reason for the last restart,
   radio, free memory, rescue image. Actions: update firmware, restart.
2. **How this box is set up** — name, time zone, role.
3. **The row it drives** — answering count, faults, supply floor, unit firmware.
4. **How this row behaves** — flap speed, line-up.
5. **The units** — a tappable grid, with a legend that says what the colours
   mean in words.
6. **Its place in the wall** — position, wall role, who leads.
7. **When something has gone wrong** — read the log, rescue image, forget
   Wi-Fi, take out of the wall.

Section 7 is last and plainly labelled, with the lede *"Rarely needed, and each
one interrupts the wall. Nothing here happens without asking you first."* It is
**not** a gated service mode — the rejected "Locked Service Bay". There is no
lock and no mode, only last position and honest copy.

## The two rules

### Load-bearing rule: settings live next to the thing they affect

Flap speed and line-up belong to **the row**. Name, time zone and role belong
to **the box**. Reflash belongs to **the units**. OTA belongs to **the box's
firmware**. There is no settings section that collects them away from what they
change.

This is what makes the rebuild a change of stance rather than a rearrangement.
**If a review proposes gathering these back into one place, that is the
rejected direction returning.**

### Boundary rule: state changes density and attention, never what exists

The `Controllers` line is present in every state and merely stops being quiet
when something needs you. A silent row still shows the text it is holding. A
row taking firmware still occupies its place on the board.

If the UI starts deciding *what is on screen* from state, it has silently
become a three-way merge instead of one architecture. State may change how loud
something is. It may never change whether it is there.

The mockup's four states — steady, clock, row-0-silent, row-1-reflashing — exist
to prove exactly this: the same wall and the same three controllers, every time.

## Rendering decisions that cleared the phone gate

A 16-wide row on a phone was the make-or-break, and it is cheap to falsify, so
it was falsified first. These three techniques are why it works and should
survive implementation:

- **One flap size for the whole wall.** Every row draws its flaps at
  `calc((100% - 15 * var(--gap)) / 16)` — the widest row's cell size — on a
  flex row that does not stretch. Five units next to sixteen therefore *read*
  as five units next to sixteen. Nothing grows to fill its line.
- **`container-type: inline-size` + `font-size: 64cqw`** per flap, so the glyph
  is legible at any cell size with no media query and no JS measurement.
- **`aspect-ratio: 5 / 7`** on the flap, with a 1 px seam at 50% — the split is
  drawn, not animated. The only motion is a 200 ms `scaleY` on a flap whose
  character actually changed, inside
  `@media (prefers-reduced-motion: no-preference)`.

## Colour and theme

**Single dark theme (#396 R3).** No `prefers-color-scheme` branch, no toggle,
no second palette. This is a documented product decision: the device is a wall
display and its console is looked at in the same room as the wall.

Within that:

- **The board is darker than the page it sits on** (`#0B0A09` on `#14120F`). It
  is an object on a surface, not the surface itself, and it never takes a
  chrome colour.
- **Healthy carries no colour at all.** Only faults, busy states and the single
  accent get ink. A wall where everything is fine should be typographic.
- One accent (`#D9A03F`), one fault colour, one busy colour. The duplicate
  `--amber` declaration dies with the old stylesheet; #397 owns the token set
  and the AA contrast floor, including the `.btn.danger` failure.

## What this needs from firmware

Listed in dependency order. Only the first is a prerequisite for the first
slice.

1. **#403 — a source field on `DisplayCommand`.** Verified absent
   (`DisplayCommand.h:57-69`): the struct carries opcode, alignment, speed,
   seq, unitAddress, value and text, and nothing else. The home screen's source
   line cannot be built honestly without it.

   Eight producing call sites across five translation units — `WebEndpoints.cpp`
   ×2 (`:325`, `:354`), `ClockTask.cpp` ×2 (`:121`, `:122`),
   `ClusterLeaderGrid.cpp` ×2 (`:152`, `:162`), `MqttService.cpp` (`:370`),
   `ClusterFollower.cpp` (`:196`). *(#403's body says `WebEndpoints.cpp` ×3;
   the third hit is a comment, not a call.)*

   Plus **two re-projection sites inside `DisplayCommand.h` itself** —
   `makeResetUnitsCommand` (`:203`) and `makeReflashUnitsCommand` (`:223`) build
   a ShowText and re-stamp the opcode, so the display returns to what was on it
   before the operation. These must carry the **restored** text's source
   forward, not mint a new one; a reflash that ends by re-showing the leader's
   text must still say the leader put it there. The same rule covers the #219
   transient overlay: it reports as the overlay while it is up, and restores the
   underlying source when it reverts.

   The distinction that matters most to an owner is
   `ClusterLeaderGrid.cpp` versus `ClusterFollower.cpp`: *I put this here*
   versus *the leader put this here* is currently indistinguishable on a
   follower.
2. **A topology snapshot** — the wall/controller model in UI terms, aggregating
   `/cluster/status|config|digest`, `/units/health` and `/system/info`.
   **Reshape the orphaned `GET /status`** rather than adding a route: it
   already aggregates settings + stats + units + cluster + OTA and has no UI
   consumer.
3. **A display-state snapshot** — one cheap read before the SSE stream opens.
4. **A recovery summary** — rescue identity, partition state, coredump
   presence. Soon, not first.

**No new Unit opcode.** `GET_EXT_DIAG 0x89` (#365) already returns reset cause,
uptime, brownout and watchdog counters, step excess, hall edges, duty, stall
bits and Vcc sag. The gap is master-level attribution and cluster modelling,
not per-unit telemetry — do not spend a 16-Nano twiboot reflash on tidiness.

## Weight

The 53.6 KB figure in the audit is a **wire transfer measurement, not a flash
ceiling**, and it has been repeated as a constraint it never was. Measured
2026-07-26: app slot 4.0 MB with firmware at 1.56 MB → **2.6 MB free**;
LittleFS `storage` 5.8 MB → **~3.3 MB free** after the 2 MB flash log and the
~0.5 MB follower image.

- **Spend the app slot (PROGMEM), not LittleFS.** PROGMEM welds the UI to the
  firmware rev, so an OTA cannot leave new firmware serving an old UI. LittleFS
  assets would buy hot-reload at the cost of that guarantee, plus an upload
  flow and a drift gate.
- **Approved spend: a subsetted mono webfont (~10 KB) for the flap glyphs.**
  Today the mirror renders SF Mono, Roboto Mono or DejaVu depending on the
  device, so the most identity-carrying element on the page is left to chance.
- Principle #9 stays a design brief: no framework, no build step beyond
  `build_assets.py`, no external assets (strict CSP, offline LAN), zero
  continuously-running idle animations, `prefers-reduced-motion` honoured.
  Restraint remains the brief — but as a choice, not as an inherited number.

## Rejected — do not re-propose

Recorded so the arc does not circle. All were rejected during the 2026-07-26
design rounds as navigation mechanisms rather than ideas:

- **Option A** — an owner surface and an operator surface as separate route
  trees. This was #399's original body and is fenced off in its rewrite.
- **Option B** — one scrolling page with accordions.
- **Option C** — the three tabs, rebalanced.
- **"Appliance Face + Locked Service Bay"** — a gated maintenance mode.

The summary that settled it: *"not a different idea by itself… still reorganise
the admin panel, just across documents instead of tabs."*

Two durable findings worth not re-deriving:

- **Organising the UI around the API is an anti-pattern.** The API is an
  evidence checklist, so that no capability is orphaned. It is not a screen
  model. Copying route shape yields the CRUD console the audit rejected.
- **The omnibus `POST /`** — text, mode, alignment, speed, name, tz and MQTT
  behind provided-flag semantics — is probably *why* today's UI is
  one-card-per-concern-with-Save. Endpoint shape dictated the IA once already.
- **Apply-on-change is wrong here**, except for reversible local preferences
  such as flap speed. Width, role, cluster membership, credentials, firmware,
  provisioning and EEPROM operations must stay consequential-looking. It is a
  wall display driven from a phone.

## Slices

**Slice one must not strand the daily-use display.** It replaces the home
screen with the wall console: live mirror, the three objects distinctly, health
and source at a glance, send and stop still working. It must answer four
questions: *what is the wall showing · who last drove it · which object is
degraded · can I still send and stop.*

Cut from slice one: unit drill-in actions, every firmware flow except status
visibility, coredump UI, cluster promote/leave/fan-out, and any generic form
system.

Sequencing on the epic: **#403 before #399's home screen**. #398 and #400 are
acceptance criteria *of* #399, not steps before it — the rebuild is ground-up,
so there is no old mechanism to consolidate first. #397 runs in parallel with
anything.

## Verification

- Re-count controls and cards on the default path; count the steps needed to
  send a message.
- Flip every mockup state (steady / clock / row-silent / reflashing) and
  confirm the same wall and the same controllers are present in all four.
- Bench on a phone. The geometry cleared this gate in the mockup; it must
  survive implementation.
- Accessibility per #400: the board is a live region, every control is
  labelled, focus is visible, `prefers-reduced-motion` is honoured.
- A controller with no row states it in words and renders no row.
- The source line names the right producer when the same wall is driven from
  browser, MQTT and clock in turn.

## Open questions

- **The unit page is not designed.** The mockup stops at the controller, with
  units as a tappable grid that leads nowhere. Level four needs its own design
  round before #399 can claim the full wall → controller → unit path. It is out
  of scope for slice one.
- **#402** — the Device role control ships inert. Finish it or take it off the
  surface; either way it must be resolved before it lands on a controller page.
- **Absorbed web-UI items** (#255–#261) target surfaces this design
  restructures, so they sequence after slice one. #255's preview half cannot be
  specified until #291 defines the paging model.
