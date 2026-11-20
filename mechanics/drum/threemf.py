"""3mf for the drum parts: one object per part, one colour, 3MF core only. Reuses the flap writer's helpers."""
import xml.etree.ElementTree as ET
from pathlib import Path
from typing import Sequence

from flaps.threemf import MATERIALS_ID, _mesh, _model_root, _write_zip

COLOUR = "black"


def add_parts(resources: ET.Element, build: ET.Element, items, tess_tol: float, first_id: int,
              pindex: int = 0) -> int:
    """items: (name, solid, (tx, ty, tz)); the shift goes into the build item. Returns the next free id."""
    for k, (name, solid, shift) in enumerate(items):
        oid = first_id + k
        obj = ET.SubElement(resources, "object", id=str(oid), type="model", name=name,
                            pid=str(MATERIALS_ID), pindex=str(pindex))
        _mesh(obj, solid, tess_tol)
        tx, ty, tz = shift
        ET.SubElement(build, "item", objectid=str(oid),
                      transform=f"1 0 0 0 1 0 0 0 1 {tx:.4f} {ty:.4f} {tz:.4f}", printable="1")
    return first_id + len(items)


def write_parts(path: Path, items: Sequence[tuple[str, object, tuple[float, float, float]]],
                tess_tol: float) -> None:
    """items: (name, solid, (tx, ty, tz)) with the solid already in place; the shift goes into the build item."""
    root, resources, build = _model_root([COLOUR])
    add_parts(resources, build, items, tess_tol, MATERIALS_ID + 1)
    _write_zip(Path(path), root)
