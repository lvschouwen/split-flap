import collections
import xml.etree.ElementTree as ET
import zipfile

import pytest

from flaps.alphabet import load_alphabet
from flaps.geometry import build_flap
from flaps.params import FlapParams
from flaps.plates import Placed
from flaps.threemf import write_plate

P = FlapParams()
NS = {"m": "http://schemas.microsoft.com/3dmanufacturing/core/2015/02"}
MEMBERS = {"[Content_Types].xml", "_rels/.rels", "3D/3dmodel.model", "Metadata/model_settings.config"}


@pytest.fixture(scope="module")
def plate(tmp_path_factory):
    a = load_alphabet()
    path = tmp_path_factory.mktemp("p") / "plate.3mf"
    placed = [(Placed(0, 5.0, 5.0), build_flap(0, a, P)), (Placed(57, 50.0, 5.0), build_flap(57, a, P))]
    write_plate(path, placed, P)
    return path, placed


def _read(path, name):
    with zipfile.ZipFile(path) as z:
        return ET.fromstring(z.read(name))


def test_archive_members(plate):
    with zipfile.ZipFile(plate[0]) as z:
        assert MEMBERS <= set(z.namelist())
        for n in MEMBERS:
            ET.fromstring(z.read(n))


def _slots(path):
    cfg = _read(path, "Metadata/model_settings.config")
    out = {}
    for obj in cfg.findall("object"):
        for part in obj.findall("part"):
            meta = {m.get("key"): m.get("value") for m in part.findall("metadata")}
            out[meta["name"]] = int(meta["extruder"])
    return out


def test_every_part_has_a_slot(plate):
    slots = _slots(plate[0])
    assert all(1 <= s <= 8 for s in slots.values())
    assert slots["flap00_body"] == 1 and slots["flap00_white"] == 2
    assert slots["flap57_red"] == 3 and slots["flap57_orange"] == 4


def test_part_ids_follow_component_order(plate):
    model = _read(plate[0], "3D/3dmodel.model")
    cfg = _read(plate[0], "Metadata/model_settings.config")
    parents = {o.get("id"): [c.get("objectid") for c in o.findall("m:components/m:component", NS)]
               for o in model.findall("m:resources/m:object", NS) if o.find("m:components", NS) is not None}
    for obj in cfg.findall("object"):
        assert [p.get("id") for p in obj.findall("part")] == parents[obj.get("id")]
    items = [i.get("objectid") for i in model.findall("m:build/m:item", NS)]
    assert sorted(items) == sorted(parents)


def test_meshes_are_closed(plate):
    model = _read(plate[0], "3D/3dmodel.model")
    meshes = model.findall("m:resources/m:object/m:mesh", NS)
    assert meshes
    for mesh in meshes:
        tris = [(int(t.get("v1")), int(t.get("v2")), int(t.get("v3"))) for t in mesh.findall("m:triangles/m:triangle", NS)]
        assert tris
        edges = collections.Counter()
        for a, b, c in tris:
            for e in ((a, b), (b, c), (c, a)):
                edges[tuple(sorted(e))] += 1
        assert set(edges.values()) == {2}


def test_items_on_bed(plate):
    model = _read(plate[0], "3D/3dmodel.model")
    objs = {o.get("id"): o for o in model.findall("m:resources/m:object", NS)}
    for item in model.findall("m:build/m:item", NS):
        t = [float(v) for v in item.get("transform").split()]
        dx, dy = t[9], t[10]
        for comp in objs[item.get("objectid")].findall("m:components/m:component", NS):
            for v in objs[comp.get("objectid")].findall("m:mesh/m:vertices/m:vertex", NS):
                x, y, z = float(v.get("x")) + dx, float(v.get("y")) + dy, float(v.get("z"))
                assert 0 <= x <= P.bed and 0 <= y <= P.bed and -1e-6 <= z <= P.thickness + 1e-6


def test_plain_mode_has_no_bambu_metadata(tmp_path, plate):
    path = tmp_path / "plain.3mf"
    write_plate(path, plate[1], P, bambu=False)
    with zipfile.ZipFile(path) as z:
        assert "Metadata/model_settings.config" not in z.namelist()
        assert b"BambuStudio" not in z.read("3D/3dmodel.model")
