# PCB v2 Review Artifacts

This directory holds artifacts from the April 2026 external review
round of the v2 PCB design package (issues #85/#86 and the follow-up
review that closed as #102).

## Contents

- **`CHATGPT_REVIEW_PROMPT.md`** — the prompt used to run the
  external review. Kept for reference.
- **`pcb_v2_handoff_for_chatgpt.zip`** — an April 2026 snapshot of
  the design package, made for the review. It **predates the review
  corrections** — do not source anything from it. It exists only as
  the record of what was reviewed.

## `handoff_pkg/` removed 2026-07-04 (#102)

The `handoff_pkg/` directory was a byte-identical (except its README)
snapshot of `PCB/v2/` made for the review. As a duplicate it would
silently diverge from the maintained tree as corrections landed, so
it was removed in the #102 doc-correction pass. The canonical tree is
**`PCB/v2/`**; the pre-removal snapshot remains available in the git
history of this file.
