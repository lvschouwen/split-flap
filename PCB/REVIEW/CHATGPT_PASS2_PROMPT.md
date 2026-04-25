# ChatGPT pass-2 review prompt

> **How to use this file:** paste the prompt block below into ChatGPT alongside the same three artifacts as pass-1 (`CHANGELOG_V1_TO_V2.md` + `pcb_v1.zip` + `pcb_v2.zip`). The `pcb_v2.zip` already contains the pass-1 outputs baked in (`REVIEW_VERIFIED`, `REVIEW_BLOCKER`, `LAYOUT_CONSTRAINT:`, `REVIEW_STRONG_OPINION:`, `REVIEW_SAFETY:` notes throughout the docs and BOMs).

## Context for whoever reads this later

Pass-1 was run against the post-#76 architecture-pivot bundle and returned: a BOM-correction pass on master + unit, layout-readiness constraints, open-issue reclassification, and a few strong-opinion / safety notes. Output applied to the design in commit `aca3026` (closes issue #80).

Pass-1 left several BOM rows as hard `CHECK` because their MPNs couldn't be cleanly verified on JLC during the review (most notably `MAX14830ETJ+`, `TPS259827YFFR`, `LMR36015AFDDA`, `INA237AIDGSR`, `TPS3839L33DBVR`, `SQJ148EP-T1_GE3`, `SM712-02HTG`, the Würth `744232601` CM choke, all RJ45 jacks). The single biggest open ask in the project is **closing those `CHECK` rows** — that is what pass-2 is for.

Architectural decisions are still locked. No schematic capture has happened yet.

---

## Prompt to paste

> I'm handing you the same three artifacts as last time (`CHANGELOG_V1_TO_V2.md` + `pcb_v1.zip` + `pcb_v2.zip`), but this is **pass 2**. Your previous output has already been applied to the design — see commit `aca3026` on branch `pcb-v2-rs485-48v` (closes issue #80).
>
> What's now baked into the bundle from your last pass:
>
> - BOM corrections you verified (master `U1 → C2941042`, `U17 → C7519`, `U13–U16 → C57928`; unit `U1 → C529331`, `U2 → C139663`, `U3 → C79815`, `U4 → C57928`, `U5 → C44377`, `U6 → C50936`, `Q_REV → C15127`) — these now carry `REVIEW_VERIFIED` / `REVIEW_VERIFIED_LCSC` notes.
> - Items you flagged as risky are now `CHECK` with explicit `REVIEW_BLOCKER:` reasoning.
> - New `BOM / sourcing gates`, `LAYOUT_CONSTRAINT:`, `REVIEW_STRONG_OPINION:`, `REVIEW_SAFETY:` subsections in each board's DECISIONS section.
> - `CHANGELOG_V1_TO_V2.md` §4 reclassified: TPS259827 DSBGA-10 manufacturability, 48 V inlet labelling, mech-freelancer / AS5600 datum, and a new factory-programming / 80-unit test-fixture flow are all promoted to **Critical**.
> - `REVIEW_COST_WARNING` near §6.
>
> **Architectural decisions remain locked** (no schematic capture yet; same review posture as last time — push back only on clear "this won't work" cases).
>
> ---
>
> ### What I want from this pass
>
> Two deliverables. Section A is the priority — if you only do one thing, do that.
>
> #### A. Resolve every remaining `CHECK` row in the BOMs *(highest priority — closes #75)*
>
> Walk through `MASTER_BOM.csv`, `UNIT_BOM.csv`, `BACKPLANE_BOM.csv` and for **every row whose `LCSC Part #` is `CHECK`**, return one of:
>
> 1. **Verified replacement**: live LCSC `Cxxxxxx` you confirmed on https://jlcpcb.com/parts, plus JLC Basic vs Extended, current stock, qty-priced unit cost (qty 5 master / qty ~20 backplane segment / qty ~80 unit), and any datasheet cross-check.
> 2. **JLC-Basic-library substitute**: drop-in equivalent that's cheaper to assemble at JLC, with a one-line justification (pinout, footprint, electrical params, package). Only if the original MPN isn't carried at all.
> 3. **Genuinely unsourceable + recommendation**: if neither the MPN nor a substitute is available at JLC, say so and recommend either (a) a different vendor route, (b) a different topology that uses JLC-friendly parts, or (c) a manual-feeder PCBA hand-assembly fallback.
>
> Particular attention to the items I flagged as biggest sourcing risks last pass:
>
> - **`MAX14830ETJ+` (master U12)** — if no clean JLC-stocked path exists, propose either: a 2-bus ESP32-S3-native UART master (your `REVIEW_STRONG_OPINION` from last pass), a different 4×UART SPI bridge IC, or accepting JLC manual-feeder for this one part.
> - **`TPS259827YFFR` (master U3–U6)** — DSBGA-10 0.4 mm pitch is a pre-fab PCBA gate at JLC. Confirm whether JLC's standard PCBA tier accepts it, or recommend committing to the placed `TPS25981QWRPVRQ1` WQFN-12 fallback footprint and dropping DSBGA from the BOM altogether.
> - **`SQJ148EP-T1_GE3` (master Q1)** — propose a JLC-Basic-tier 100 V N-MOSFET with R_DS(on) ≤ 15 mΩ in a thermally-comparable package if the Vishay part isn't sourceable.
> - **`SM712-02HTG` (unit D2)** — protection-critical; if the exact Semtech part isn't on JLC, recommend a specific RS-485-rated TVS array with the right working-voltage class.
>
> Return one **paste-ready CSV excerpt per BOM**, in the existing 13-column schema (`Designator, Comment, Footprint, LCSC Part #, MPN, Manufacturer, Qty, JLC_Tier, Populate, EstEUR, DatasheetURL, Notes, BomType`). Notes column should append `REVIEW_PASS2_VERIFIED:` or `REVIEW_PASS2_SUBSTITUTE:` so I can grep them and apply them mechanically.
>
> **Re-sum each BOM** against the headline cost claims (`~€80/master`, `~€7.50/backplane segment`, `~€8.90/unit`) using the resolved C-numbers and return the corrected totals at qty 5 / qty ~20 / qty ~80. Flag any per-board total off by more than 15 %.
>
> #### B. Layout-readiness audit *(secondary — fast pass)*
>
> Each board's DECISIONS section now carries a `LAYOUT_CONSTRAINT: ... pre-layout checklist` subsection that I added based on your last pass. Take one quick look at each and tell me:
>
> 1. Is anything still missing that a freelance KiCad layout engineer would bounce the spec back asking for? (e.g. impedance targets, copper weights, stackup symmetry, fiducial count + placement, panelization tab/mouse-bite spec, DRC class, drill-to-copper minima, controlled-impedance callouts.)
> 2. **Master**: 130 × 100 mm, 4-layer ENIG, ESP32-S3 antenna keep-out, controlled-impedance RS-485 + USB. Is the new `LAYOUT_CONSTRAINT` checklist self-sufficient for tape-out?
> 3. **Backplane**: 320 × 35 mm × 4 segments, 2-layer HASL, segment-joint mechanical reliability called out. Does the `REVIEW_STRONG_OPINION: harness over rigid joints` warrant any concrete layout consequence (e.g. pre-drawing wire-harness landing pads as DNP), or is it purely a mechanical-freelancer concern?
> 4. **Unit**: 75 × 35 mm, 2-layer HASL, AS5600 datum problem flagged. The unit explicitly **is not layout-ready** until the mech freelancer signs off the AS5600 STEP — confirm or push back on whether that gate is sufficient.
>
> Keep section B short (under 500 words). Section A is the bulk of the deliverable.
>
> ---
>
> ### Format
>
> Lead with section A's three CSV excerpts (master / backplane / unit). Then the per-BOM cost re-sum. Then section B as a short bulleted list. Strong opinions over polite hedging — same posture as last pass.

---

## After ChatGPT returns

Apply the output mechanically:

1. Grep for `REVIEW_PASS2_VERIFIED:` and `REVIEW_PASS2_SUBSTITUTE:` in ChatGPT's reply.
2. Update the matching rows in `PCB/v2/MASTER_BOM.csv`, `PCB/v2/UNIT_BOM.csv`, `PCB/v2/BACKPLANE_BOM.csv`. Preserve the 13-column schema.
3. Update derivative docs (`POWER_DESIGN.md`, `DIGITAL_DESIGN.md`) where the same C-number appears in a table.
4. Update `MASTER_DECISIONS.md` "BOM / sourcing gates" subsection: move resolved items off the `CHECK` list.
5. Re-run the regenerate-bundle step (concatenate per-board mega-docs, rebuild `pcb_v2.zip`).
6. Commit referencing #75 and the new pass-2 issue.

If pass-2 still leaves items unresolved (e.g. JLC really doesn't carry MAX14830 in any package), the next decision point is architectural — either accept manual feeder cost, swap to a 2-bus master, or pick a different SPI-UART bridge IC.
