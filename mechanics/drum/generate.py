"""Write drum and frame parts as 3mf. Run from mechanics/: python -m drum.generate --test-assembly"""
import argparse
import sys
from pathlib import Path
from typing import Sequence


from drum import assembly as asm
from drum.cad import box, cyl
from drum.layout import shelf_pack
from drum.params import DrumParams
from drum.threemf import write_parts

BED, BED_MARGIN, SPACING = 256.0, 5.0, 3.0
COUPON_STEPS = 8
COUPON_HOLES = 8


def _parse_set(items: Sequence[str]) -> dict[str, float]:
    out = {}
    for item in items:
        key, sep, value = item.partition("=")
        if not sep:
            raise ValueError(f"--set needs KEY=VALUE, got {item!r}")
        out[key] = float(value)
    return out


def print_items(named: Sequence[tuple[str, object]], tol: float) -> list[tuple[list, list]]:
    """Pack named print-frame solids; returns per plate the (name, solid, shift) items."""
    boxes = [s.bounding_box() for _, s in named]
    sizes = [(b.size.X, b.size.Y) for b in boxes]
    corners, idx = shelf_pack(sizes, BED, BED_MARGIN, SPACING)
    plates = []
    for cs, ids in zip(corners, idx):
        plates.append([(named[i][0], named[i][1], (x - boxes[i].min.X, y - boxes[i].min.Y, -boxes[i].min.Z))
                       for (x, y), i in zip(cs, ids)])
    return plates


def _write_plates(out: Path, stem: str, named, tol: float) -> list[Path]:
    paths = []
    for n, items in enumerate(print_items(named, tol), 1):
        path = out / f"{stem}-{n}.3mf"
        write_parts(path, items, tol)
        print(f"{path}: {', '.join(name for name, _, _ in items)}")
        paths.append(path)
    return paths


def coupon(p: DrumParams):
    """Eight blocks of eight pivot holes at the real chord, hole_slop 0.0 to 0.4 (section 2)."""
    from drum import feasibility as fe
    c = fe.chord(p.count, p.r_p)
    out = []
    for k in range(COUPON_STEPS):
        slop = 0.4 * k / (COUPON_STEPS - 1)
        w = COUPON_HOLES * c + 2 * p.rim_web
        blk = box(0, w, 0, p.hole_d + 2 * p.rim_web, 0, p.disc_t)
        for i in range(COUPON_HOLES):
            blk = blk - cyl((p.flap.pin_d + slop) / 2, p.disc_t + 2, -1, p.rim_web + c * (i + 0.5), p.rim_web + p.hole_d / 2)
        out.append((f"slop{int(round(slop * 100)):02d}", blk))
    return out


def main(argv: Sequence[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--coupon", action="store_true", help="hole-size coupon plate")
    ap.add_argument("--test-assembly", action="store_true", help="drum + frame for the hand-turn test")
    ap.add_argument("--full", action="store_true", help="test assembly plus the motor adapter sleeve")
    ap.add_argument("--assembled", action="store_true", help="one 3mf with every part in place (inspection)")
    ap.add_argument("--set", action="append", default=[], metavar="KEY=VALUE", help="override a DrumParams field")
    ap.add_argument("--out", default="out", type=Path)
    a = ap.parse_args(argv)
    if not (a.coupon or a.test_assembly or a.full or a.assembled):
        ap.error("pick --coupon, --test-assembly, --full or --assembled")
    p = DrumParams().with_overrides(**_parse_set(a.set))
    tol = p.tess_tol
    if a.coupon:
        _write_plates(a.out, "coupon", coupon(p), tol)
    if a.test_assembly or a.full:
        named = [(q.name, q.solid) for q in asm.parts(p) if a.full or q.essential]
        _write_plates(a.out, "full" if a.full else "test-assembly", named, tol)
    if a.assembled:
        write_parts(a.out / "assembled.3mf", [(n, s, (0.0, 0.0, 0.0)) for n, s in asm.assembled(p)], tol)
        print(f"{a.out / 'assembled.3mf'}: every part in place")
    return 0


if __name__ == "__main__":
    sys.exit(main())
