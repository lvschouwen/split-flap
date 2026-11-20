"""Write flap plates as 3mf. Run from mechanics/: python -m flaps.generate --test-plate"""
import argparse
import sys
from pathlib import Path
from typing import Sequence

from flaps.alphabet import FLAP_COUNT, Entry, load_alphabet
from flaps.geometry import FlapParts, build_flap
from flaps.params import FlapParams
from flaps.plates import Placed, grid, pack
from flaps.threemf import write_plate

# Fronts that stress the printer: thin strokes, small dots, wide glyphs, the heart.
TEST_FRONTS = ['%', '"', '.', ',', ':', '@', 'M', 'W', '8']
TEST_COLOUR_FLAP = 57
VARIANT_FLAP = 1
# Thickness variants bracket the 0.8 default (pin_d caps thickness at 1.0); inlay depth stays below half.
VARIANTS = (("inlay03", {"inlay_depth": 0.3}),
            ("t07", {"thickness": 0.7}),
            ("t10", {"thickness": 1.0, "inlay_depth": 0.4}))


def _parse_set(items: Sequence[str]) -> dict[str, float]:
    out = {}
    for item in items:
        key, sep, value = item.partition("=")
        if not sep:
            raise ValueError(f"--set needs KEY=VALUE, got {item!r}")
        out[key] = float(value)
    return out


def _index_of(alphabet: Sequence[Entry], entry: Entry) -> int:
    return alphabet.index(entry)


def _test_plate(alphabet, p: FlapParams) -> tuple[list[tuple[int, FlapParts]], list[str]]:
    flaps = [_index_of(alphabet, Entry("char", c)) for c in TEST_FRONTS]
    flaps += [_index_of(alphabet, Entry("heart", "")), TEST_COLOUR_FLAP]
    built = [(i, build_flap(i, alphabet, p)) for i in flaps]
    names = [f"flap{i:02d}" for i in flaps]
    for suffix, kw in VARIANTS:
        built.append((VARIANT_FLAP, build_flap(VARIANT_FLAP, alphabet, p.with_overrides(**kw))))
        names.append(f"flap{VARIANT_FLAP:02d}_{suffix}")
    return built, names


def _write(path: Path, built, names, placed: Sequence[Placed], p: FlapParams) -> None:
    write_plate(path, [(pl, parts) for pl, (_, parts) in zip(placed, built)],
                p, names=names)
    colours = sorted({c for _, parts in built for c in parts.inlays} | {"black"})
    print(f"{path}: {len(built)} flaps, colours {', '.join(colours)}")


def _full(alphabet, p: FlapParams, out: Path) -> None:
    # One file: the slicer's auto-arrange spreads it over as many plates as it needs.
    built = [(i, build_flap(i, alphabet, p)) for i in range(FLAP_COUNT)]
    _write(out / "drum.3mf", built, [f"flap{i:02d}" for i, _ in built], grid([i for i, _ in built], p), p)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    mode = ap.add_mutually_exclusive_group(required=True)
    mode.add_argument("--test-plate", action="store_true")
    mode.add_argument("--full", action="store_true", help="all 64 flaps of one drum in out/drum.3mf")
    ap.add_argument("--out", type=Path, default=Path(__file__).with_name("out"))
    ap.add_argument("--set", action="append", default=[], metavar="KEY=VALUE")
    args = ap.parse_args(argv)
    try:
        p = FlapParams().with_overrides(**_parse_set(args.set))
    except ValueError as e:
        print(f"generate: {e}", file=sys.stderr)
        return 2
    alphabet = load_alphabet()
    if args.test_plate:
        built, names = _test_plate(alphabet, p)
        (plate,) = pack([i for i, _ in built], p, tower=True)
        _write(args.out / "test-plate.3mf", built, names, plate, p)
    else:
        _full(alphabet, p, args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
