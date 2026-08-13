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
