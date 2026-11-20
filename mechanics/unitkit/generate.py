"""One 3mf with every printed part of a unit: 64 flaps (one part per colour) and the drum/frame parts.

Run from mechanics/: python -m unitkit --out out/unit.3mf. The grid runs past the bed in y;
the slicer's Arrange spreads it over plates. Geometry comes from flaps/ and drum/ unchanged.
"""
import argparse
import sys
from pathlib import Path
from typing import Sequence

from drum import assembly as asm
from drum.generate import BED, print_items
from drum.params import DrumParams
from drum.threemf import add_parts
from flaps.alphabet import FLAP_COUNT, load_alphabet
from flaps.geometry import build_flap
from flaps.params import FlapParams
from flaps.plates import grid
from flaps.threemf import MATERIALS_ID, _model_root, _write_zip, add_flaps, flap_colours

# drum Part name -> object name in the kit
OBJECT_NAMES = {"drum-a": "drum_half_a", "drum-b": "drum_half_b", "sleeve": "sleeve",
                "wall-motor": "wall_motor", "wall-far": "wall_far", "bearing-ring": "bearing_ring",
                "rail-bottom": "rail_bottom", "rail-top": "rail_top", "front-plate": "display_plate",
                "back-plate": "back_plate", "stop-lip": "stop_lip"}


def build_kit(path: Path, fp: FlapParams, dp: DrumParams) -> tuple[int, float, float]:
    """Write the kit; returns (object count, grid x extent, grid y extent) in mm."""
    alphabet = load_alphabet()
    flaps = [build_flap(i, alphabet, fp) for i in range(FLAP_COUNT)]
    placed = list(zip(grid(list(range(FLAP_COUNT)), fp), flaps))
    colours = flap_colours(placed)
    root, resources, build = _model_root(colours)
    next_id = add_flaps(resources, build, placed, fp, colours, MATERIALS_ID + 1)
    y_end = max(w.y for w, _ in placed) + fp.height
    named = [(OBJECT_NAMES[q.name], q.solid) for q in asm.parts(dp)]
    items = []
    for n, plate in enumerate(print_items(named, dp.tess_tol)):
        items += [(name, solid, (x, y + y_end + fp.spacing + n * BED, z)) for name, solid, (x, y, z) in plate]
    add_parts(resources, build, items, dp.tess_tol, next_id, pindex=colours.index("black"))
    _write_zip(path, root)
    return len(build), max(w.x for w, _ in placed) + fp.width, y_end + fp.spacing + (n + 1) * BED


def main(argv: Sequence[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", type=Path, default=Path("out/unit.3mf"))
    ap.add_argument("--tess-tol", type=float, help="override both generators' tessellation tolerance")
    a = ap.parse_args(argv)
    fp, dp = FlapParams(), DrumParams()
    if a.tess_tol is not None:
        fp, dp = fp.with_overrides(tess_tol=a.tess_tol), dp.with_overrides(tess_tol=a.tess_tol)
    count, w, h = build_kit(a.out, fp, dp)
    print(f"{a.out}: {count} objects, grid {w:.0f} x {h:.0f} mm")
    return 0


if __name__ == "__main__":
    sys.exit(main())
