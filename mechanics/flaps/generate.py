"""Write flap plates as 3mf. Run from mechanics/: python -m flaps.generate --test-plate"""
import argparse
import sys
from pathlib import Path
from typing import Sequence

from flaps.alphabet import Entry, load_alphabet
from flaps.geometry import FlapParts, build_flap
from flaps.params import FlapParams
from flaps.plates import Placed, pack, split_by_colour
from flaps.threemf import write_plate

# Fronts that stress the printer: thin strokes, small dots, wide glyphs, the heart.
TEST_FRONTS = ['%', '"', '.', ',', ':', '@', 'M', 'W', '8']
TEST_COLOUR_FLAP = 57
VARIANT_FLAP = 1
VARIANTS = (("inlay03", {"inlay_depth": 0.3}), ("t12", {"thickness": 1.2}))


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


def _write(path: Path, built, names, p: FlapParams, tower: bool) -> None:
    (plate,) = pack([i for i, _ in built], p, tower=tower)
    write_plate(path, [(Placed(pl.flap, pl.x, pl.y), parts) for pl, (_, parts) in zip(plate, built)],
                p, names=names)
    colours = sorted({c for _, parts in built for c in parts.inlays} | {"black"})
    print(f"{path}: {len(built)} flaps, colours {', '.join(colours)}")


def _full(alphabet, p: FlapParams, out: Path) -> None:
    plain, coloured = split_by_colour(alphabet)
    groups = [(plate, False) for plate in pack(plain, p, tower=False)]
    groups += [(plate, True) for plate in pack(coloured, p, tower=True)]
    for k, (plate, tower) in enumerate(groups, start=1):
        built = [(pl.flap, build_flap(pl.flap, alphabet, p)) for pl in plate]
        _write(out / f"drum-plate-{k}.3mf", built, [f"flap{i:02d}" for i, _ in built], p, tower)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    mode = ap.add_mutually_exclusive_group(required=True)
    mode.add_argument("--test-plate", action="store_true")
    mode.add_argument("--full", action="store_true", help="every plate for one drum")
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
        _write(args.out / "test-plate.3mf", built, names, p, tower=True)
    else:
        _full(alphabet, p, args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
