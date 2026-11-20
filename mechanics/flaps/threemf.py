"""3mf plates: one assembly per flap, one mesh object per part (body + each colour).

Bambu Studio reads Metadata/model_settings.config for any 3mf: a <part id> is
the component's mesh-object id, listed in component order, and its
"extruder" metadata is the AMS slot. A parent object's id follows its parts'.
With bambu=False only the 3MF core is written (assign filaments by hand).
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
BAMBU_NS = "http://schemas.bambulab.com/package/2021"
APPLICATION = "BambuStudio-02.00.00.00"
_WELD_DIGITS = 6

CONTENT_TYPES = (
    '<?xml version="1.0" encoding="UTF-8"?>\n'
    '<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">'
    '<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>'
    '<Default Extension="model" ContentType="application/vnd.ms-package.3dmanufacturing-3dmodel+xml"/>'
    '<Default Extension="config" ContentType="text/xml"/>'
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


def _flap_parts(parts: FlapParts) -> list[tuple[str, int, object]]:
    """(part suffix, AMS slot, solid), body first, colours in slot order."""
    colours = sorted(parts.inlays, key=COLOUR_SLOTS.__getitem__)
    return [("body", COLOUR_SLOTS["black"], parts.body)] + \
        [(c, COLOUR_SLOTS[c], parts.inlays[c]) for c in colours]


def _settings_part(obj: ET.Element, part_id: int, name: str, slot: int) -> None:
    part = ET.SubElement(obj, "part", id=str(part_id), subtype="normal_part")
    ET.SubElement(part, "metadata", key="name", value=name)
    ET.SubElement(part, "metadata", key="extruder", value=str(slot))


def _model_root(bambu: bool) -> tuple[ET.Element, ET.Element, ET.Element]:
    root = ET.Element("model", {"unit": "millimeter", "xml:lang": "en-US", "xmlns": CORE_NS})
    if bambu:
        root.set("xmlns:BambuStudio", BAMBU_NS)
        ET.SubElement(root, "metadata", name="Application").text = APPLICATION
        ET.SubElement(root, "metadata", name="BambuStudio:3mfVersion").text = "1"
    return root, ET.SubElement(root, "resources"), ET.SubElement(root, "build")


def write_plate(path: Path, placed: Sequence[tuple[Placed, FlapParts]], p: FlapParams,
                bambu: bool = True, names: Sequence[str] | None = None) -> None:
    """names: optional per-flap object names (default flapNN)."""
    root, resources, build = _model_root(bambu)
    config = ET.Element("config")
    next_id = 1
    for k, (where, parts) in enumerate(placed):
        name = names[k] if names else f"flap{where.flap:02d}"
        part_ids = []
        settings = ET.SubElement(config, "object")
        for suffix, slot, solid in _flap_parts(parts):
            obj = ET.SubElement(resources, "object", id=str(next_id), type="model",
                                name=f"{name}_{suffix}")
            _mesh(obj, solid, p.tess_tol)
            _settings_part(settings, next_id, f"{name}_{suffix}", slot)
            part_ids.append(next_id)
            next_id += 1
        parent = ET.SubElement(resources, "object", id=str(next_id), type="model", name=name)
        comps = ET.SubElement(parent, "components")
        for pid in part_ids:
            ET.SubElement(comps, "component", objectid=str(pid))
        settings.set("id", str(next_id))
        ET.SubElement(settings, "metadata", key="name", value=name)
        # The flap's min corner (outline min x = -width/2, y = 0) lands on the placed corner.
        tx, ty = where.x + p.width / 2, where.y
        ET.SubElement(build, "item", objectid=str(next_id),
                      transform=f"1 0 0 0 1 0 0 0 1 {tx:.4f} {ty:.4f} 0", printable="1")
        next_id += 1
    _write_zip(Path(path), root, config if bambu else None)


def _write_zip(path: Path, model: ET.Element, config: ET.Element | None) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("[Content_Types].xml", CONTENT_TYPES)
        z.writestr("_rels/.rels", RELS)
        z.writestr("3D/3dmodel.model", _xml(model))
        if config is not None:
            z.writestr("Metadata/model_settings.config", _xml(config))


def _xml(el: ET.Element) -> bytes:
    return b'<?xml version="1.0" encoding="UTF-8"?>\n' + ET.tostring(el, encoding="utf-8")
