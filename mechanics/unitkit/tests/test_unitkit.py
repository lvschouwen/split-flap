import re
import xml.etree.ElementTree as ET
import zipfile

import pytest

from flaps.alphabet import FLAP_COUNT
from flaps.params import FlapParams
from drum.params import DrumParams
from unitkit.generate import OBJECT_NAMES, build_kit

NS = {"m": "http://schemas.microsoft.com/3dmanufacturing/core/2015/02"}
DRUM_NAMES = set(OBJECT_NAMES.values())
FLAP_NAMES = {f"flap{i:02d}" for i in range(FLAP_COUNT)}


@pytest.fixture(scope="module")
def kit(tmp_path_factory):
    path = tmp_path_factory.mktemp("k") / "unit.3mf"
    fp, dp = FlapParams().with_overrides(tess_tol=0.2), DrumParams().with_overrides(tess_tol=0.2)
    stats = build_kit(path, fp, dp)
    with zipfile.ZipFile(path) as z:
        root = ET.fromstring(z.read("3D/3dmodel.model"))
    objects = {o.get("id"): o for o in root.findall(".//m:object", NS)}
    colours = [b.get("name") for b in root.findall(".//m:basematerials/m:base", NS)]
    return root, objects, colours, stats


def _min_z(obj, objects, shift_z):
    zs = [float(v.get("z")) for v in obj.findall(".//m:vertex", NS)]
    for c in obj.findall(".//m:component", NS):
        zs.append(_min_z(objects[c.get("objectid")], objects, 0.0))
    return min(zs) + shift_z


def test_object_names_are_exactly_the_kit(kit):
    root, objects, _, (count, _, _) = kit
    items = [objects[i.get("objectid")].get("name") for i in root.findall(".//m:build/m:item", NS)]
    assert len(items) == count == FLAP_COUNT + len(DRUM_NAMES)
    assert set(items) == FLAP_NAMES | DRUM_NAMES


def test_drum_objects_are_black(kit):
    root, objects, colours, _ = kit
    for i in root.findall(".//m:build/m:item", NS):
        o = objects[i.get("objectid")]
        if o.get("name") in DRUM_NAMES:
            assert colours[int(o.get("pindex"))] == "black"


def test_flap_parts_have_a_colour_and_body_is_black(kit):
    root, objects, colours, _ = kit
    assert colours[0] == "black" and len(colours) == len(set(colours))
    for i in root.findall(".//m:build/m:item", NS):
        o = objects[i.get("objectid")]
        if o.get("name") not in FLAP_NAMES:
            continue
        parts = [objects[c.get("objectid")] for c in o.findall(".//m:component", NS)]
        assert colours[int(parts[0].get("pindex"))] == "black"
        for part in parts[1:]:
            assert part.get("name").split("_", 1)[1] == colours[int(part.get("pindex"))]


def test_every_item_rests_on_or_above_the_bed(kit):
    root, objects, _, _ = kit
    for i in root.findall(".//m:build/m:item", NS):
        shift_z = float(i.get("transform").split()[11])
        assert _min_z(objects[i.get("objectid")], objects, shift_z) >= -1e-3


def test_grid_exceeds_one_bed(kit):
    _, _, _, (_, w, h) = kit
    assert w <= 256 and h > 256
