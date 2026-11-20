"""3mf plates: one assembly per flap, one mesh object per part (body + each colour).

3MF core only: each part object carries pid/pindex into one <basematerials>
group, listed in AMS slot order. No Bambu metadata on purpose: Studio treats a
"BambuStudio-" stamp without a full project_settings.config as an old project
and loads geometry only, while a third-party file keeps its parts and opens the
colour-to-filament dialog. Colours are matched by hand there, not by slot number.
"""
import zipfile
import xml.etree.ElementTree as ET
from pathlib import Path
from typing import Sequence

from flaps.alphabet import COLOUR_SLOTS
from flaps.geometry import FlapParts
from flaps.params import FlapParams
from flaps.plates import Placed

CORE_NS = "http://schemas.microsoft.com/3dmanufacturing/core/2015/02"
MATERIALS_ID = 1
# Close to Bambu PLA Basic, so Studio's colour dialog suggests the right filament.
DISPLAY_COLOURS = {"black": "#000000", "white": "#FFFFFF", "red": "#C12E1F",
                   "orange": "#FF6A13", "yellow": "#F4EE2A", "green": "#00AE42",
                   "blue": "#0A2989", "violet": "#5E43B7"}
_WELD_DIGITS = 6

CONTENT_TYPES = (
    '<?xml version="1.0" encoding="UTF-8"?>\n'
    '<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">'
    '<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>'
    '<Default Extension="model" ContentType="application/vnd.ms-package.3dmanufacturing-3dmodel+xml"/>'
    '</Types>')
RELS = (
    '<?xml version="1.0" encoding="UTF-8"?>\n'
    '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
    '<Relationship Target="/3D/3dmodel.model" Id="rel-1" '
    'Type="http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel"/>'
    '</Relationships>')


def _mesh(parent: ET.Element, shape, tol: float) -> None:
    verts, tris = shape.tessellate(tol)
    index: dict[tuple, int] = {}
    remap = []
    for v in verts:
        key = (round(v.X, _WELD_DIGITS), round(v.Y, _WELD_DIGITS), round(v.Z, _WELD_DIGITS))
        remap.append(index.setdefault(key, len(index)))
    mesh = ET.SubElement(parent, "mesh")
    vs = ET.SubElement(mesh, "vertices")
    for x, y, z in index:
        ET.SubElement(vs, "vertex", x=f"{x:.6f}", y=f"{y:.6f}", z=f"{z:.6f}")
    ts = ET.SubElement(mesh, "triangles")
    for a, b, c in tris:
        a, b, c = remap[a], remap[b], remap[c]
        if len({a, b, c}) == 3:
            ET.SubElement(ts, "triangle", v1=str(a), v2=str(b), v3=str(c))


def _flap_parts(parts: FlapParts) -> list[tuple[str, object]]:
    """(colour, solid), the black body first, inlays in slot order."""
    colours = sorted(parts.inlays, key=COLOUR_SLOTS.__getitem__)
    return [("black", parts.body)] + [(c, parts.inlays[c]) for c in colours]


def _model_root(colours: Sequence[str]) -> tuple[ET.Element, ET.Element, ET.Element]:
    root = ET.Element("model", {"unit": "millimeter", "xml:lang": "en-US", "xmlns": CORE_NS})
    resources = ET.SubElement(root, "resources")
    group = ET.SubElement(resources, "basematerials", id=str(MATERIALS_ID))
    for c in colours:
        ET.SubElement(group, "base", name=c, displaycolor=DISPLAY_COLOURS[c] + "FF")
    return root, resources, ET.SubElement(root, "build")


def flap_colours(flaps: Sequence[tuple[Placed, FlapParts]]) -> list[str]:
    """Colours used by these flaps in AMS slot order (black first)."""
    return sorted({c for _, parts in flaps for c, _ in _flap_parts(parts)}, key=COLOUR_SLOTS.__getitem__)


def add_flaps(resources: ET.Element, build: ET.Element, placed: Sequence[tuple[Placed, FlapParts]],
              p: FlapParams, colours: Sequence[str], next_id: int,
              names: Sequence[str] | None = None) -> int:
    """Add one assembly object per flap (one part per colour) and its build item; returns the next free id."""
    for k, (where, parts) in enumerate(placed):
        name = names[k] if names else f"flap{where.flap:02d}"
        part_ids = []
        for colour, solid in _flap_parts(parts):
            obj = ET.SubElement(resources, "object", id=str(next_id), type="model",
                                name=f"{name}_{'body' if not part_ids else colour}",
                                pid=str(MATERIALS_ID), pindex=str(colours.index(colour)))
            _mesh(obj, solid, p.tess_tol)
            part_ids.append(next_id)
            next_id += 1
        parent = ET.SubElement(resources, "object", id=str(next_id), type="model", name=name)
        comps = ET.SubElement(parent, "components")
        for pid in part_ids:
            ET.SubElement(comps, "component", objectid=str(pid))
        # The flap's min corner (outline min x = -width/2, y = 0) lands on the placed corner.
        tx, ty = where.x + p.width / 2, where.y
        ET.SubElement(build, "item", objectid=str(next_id),
                      transform=f"1 0 0 0 1 0 0 0 1 {tx:.4f} {ty:.4f} 0", printable="1")
        next_id += 1
    return next_id


def write_plate(path: Path, placed: Sequence[tuple[Placed, FlapParts]], p: FlapParams,
                names: Sequence[str] | None = None) -> None:
    """names: optional per-flap object names (default flapNN)."""
    colours = flap_colours(placed)
    root, resources, build = _model_root(colours)
    add_flaps(resources, build, placed, p, colours, MATERIALS_ID + 1, names)
    _write_zip(Path(path), root)


def _write_zip(path: Path, model: ET.Element) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("[Content_Types].xml", CONTENT_TYPES)
        z.writestr("_rels/.rels", RELS)
        z.writestr("3D/3dmodel.model", _xml(model))


def _xml(el: ET.Element) -> bytes:
    return b'<?xml version="1.0" encoding="UTF-8"?>\n' + ET.tostring(el, encoding="utf-8")
