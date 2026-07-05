# ChatGPT review prompt — split-flap v2 PCB hand-off package

Copy everything below the `---` line into ChatGPT (web), upload
`pcb_v2_handoff_for_chatgpt.zip` alongside it.

---

I'm about to send this PCB design package to a freelance PCB designer (Fiverr) to produce schematics, layout, and Gerbers for a hobby split-flap display. **Three boards**: a master, 64 unit boards, 8 DIN-rail bus boards. **One 12 V brick**, four RS-485 buses (one per row of 16 units), ESP32-S3 master, STM32G030 per unit. The package has already been through one independent review and a datasheet pin-numbering sweep — most known issues are fixed. I want a final blind review from you before paying a freelancer.

The zip contains 10 files:
- `README.md` — top-level overview, scope, file index
- `EASYEDA_HANDOFF.md` — hand-off cover page with status + 5 "must escalate before fab" items
- `ARCHITECTURE.md` — system block diagram, power flow, addressing scheme
- `OPEN_DECISIONS.md` — deferred / build-time decisions (currently 5 entries)
- `SCHEMATIC_MASTER.md` — full master PCB spec (ESP32-S3, SC16IS740, 4× SN65HVD75, USB-C, 15 A power input)
- `SCHEMATIC_UNIT.md` — full unit PCB spec (STM32G030, TPL7407L stepper, RS-485, off-board hall via 3-pin connector, 4 pogo pins on underside)
- `SCHEMATIC_BUS.md` — full bus PCB spec (300 × 30 mm DIN-rail strip, 4 traces, 8 station landings)
- `BUS_PCB.md` — bus design notes (cables, polarization key, terminator plug)
- `UNIT.md` — unit design notes (motor, hall, RS-485 path)
- `BOMS.csv` — combined BOM for all three boards (refdes, qty, value, footprint, MPN, LCSC, notes)

## What I want from you

A **blind read** in the role of a competent freelance PCB designer who has just been hired and is reading the package for the first time. Find anything that would make you email me back with questions, or that would result in a board that doesn't work as intended.

### Find these
1. **Showstoppers** — missing component, undefined net, ambiguous pin assignment, wrong package, wrong LCSC code, wrong polarity, wrong footprint, electrically broken topology.
2. **Cross-doc contradictions** — same fact stated differently in two files.
3. **Mechanical gaps** — missing dimensions, missing keep-outs, undefined clearances.
4. **Safety / polarity** — places where reverse-mounting or wrong-polarity install would cause damage and the doc doesn't catch it.
5. **Footprint / library risks** — places where the EasyEDA library default for a part (especially WROOM-1, USB-C, SOT-23-6 ESD, SC16IS740) is likely wrong vs. the manufacturer datasheet.
6. **Power / thermal sanity** — current ratings, wire gauges, copper widths, LDO dissipation budgets.
7. **Anything a competent PCB designer would ask back about before quoting or starting layout.**

### Don't waste effort on
- Firmware concerns (out of scope; intentionally excluded).
- Cost optimisation (this is a hobby project; LCSC starting points are intentional).
- Style / wording polish (only flag if it changes meaning).

## Empowerment

The package documents 5 "must escalate before fab" items at the top of `EASYEDA_HANDOFF.md`. For each one (and for any new finding), please **make a recommendation when the technical answer is clear**, and **ask me a direct question only when the choice depends on something you can't see in the docs** (e.g. my chassis, my budget, my risk tolerance).

Example of what I want:
- *"For escalation #1 (row connector LCSC): I recommend JST-XH 4-pin (B4B-XH-A, LCSC C158012). Same family as the unit's stepper output (J2), keyed crimp housing, in stock. Ok to lock?"* ← **good** — decisive, specific, asks for one-word confirmation.
- *"For escalation #1: there are several options."* ← **bad** — kicks the decision back to me without progressing it.

If you'd lock something in but want me to confirm, ask: **"OK to lock X as Y?"** and I'll answer yes/no.

## Output format

Plain markdown. Two sections:

### 1. Critical (must-fix before hand-off)
Bulleted; each finding has:
- **What** — one-line summary
- **Where** — file + section/line reference
- **Why it matters** — one sentence
- **Fix** — one sentence, with a specific recommendation. End with either "**OK to apply?**" or, if you genuinely can't decide without info from me, a single direct question.

### 2. Should-fix (would reduce questions during execution)
Same format, lower bar.

### 3. Decisions you've already made
List anything where you took a clear technical position, so I can review them in one place.

### 4. Questions for me
Any genuinely user-dependent decisions (chassis, budget, mechanical specs you can't infer) — at most 5, prioritised.

## Bottom line

End with a one-sentence verdict: "**The package is at X% — fix the items above and it's hand-off ready**", with X being your honest call.

Be honest. The previous review came back at 94% and identified 5 real blockers, all of which have been fixed (see commit history if I share it). I'm hoping you find ≤3 critical items and the package lands at ≥97%. If you find more, that's still useful — better to find them now than after I pay a freelancer.

Please proceed.
