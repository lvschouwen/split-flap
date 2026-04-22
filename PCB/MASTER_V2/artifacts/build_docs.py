#!/usr/bin/env python3
"""Build combined.md from README + SCHEMATIC + PINOUT + BOM (CSV→MD table)."""
import csv
from pathlib import Path

BASE = Path(__file__).resolve().parent.parent

def read(p): return (BASE / p).read_text()

def csv_to_md(path):
    rows = list(csv.reader((BASE / path).read_text().splitlines()))
    header, body = rows[0], rows[1:]
    out = ["| " + " | ".join(header) + " |",
           "|" + "|".join(["---"] * len(header)) + "|"]
    for r in body:
        r = [c.replace("|", "\\|") for c in r]
        out.append("| " + " | ".join(r) + " |")
    return "\n".join(out)

parts = [
    "---",
    "title: 'Master v2 Rev B — PCB design handoff'",
    "subtitle: 'ESP32-S3 + ESP32-H2 + TCA9548A carrier · 100 × 70 mm · 4-layer'",
    "date: 'generated from repo — commit current HEAD'",
    "geometry: margin=18mm",
    "fontsize: 10pt",
    "mainfont: 'DejaVu Sans'",
    "monofont: 'DejaVu Sans Mono'",
    "---",
    "",
    "# Part 1 — README",
    "",
    read("README.md"),
    "",
    "\\newpage",
    "",
    "# Part 2 — SCHEMATIC",
    "",
    read("SCHEMATIC.md"),
    "",
    "\\newpage",
    "",
    "# Part 3 — PINOUT",
    "",
    read("PINOUT.md"),
    "",
    "\\newpage",
    "",
    "# Part 4 — BOM",
    "",
    csv_to_md("BOM.csv"),
    "",
]

out = BASE / "artifacts" / "combined.md"
out.write_text("\n".join(parts))
print(f"wrote {out} · {out.stat().st_size} bytes")
