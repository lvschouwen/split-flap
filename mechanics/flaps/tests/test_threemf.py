import collections
import re
import xml.etree.ElementTree as ET
import zipfile

import pytest

from flaps.alphabet import COLOUR_SLOTS, load_alphabet
from flaps.geometry import build_flap
from flaps.params import FlapParams
from flaps.plates import Placed
from flaps.threemf import write_plate

P = FlapParams()
NS = {"m": "http://schemas.microsoft.com/3dmanufacturing/core/2015/02"}
MEMBERS = {"[Content_Types].xml", "_rels/.rels", "3D/3dmodel.model"}


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


def _part_colours(path):
    """part name -> colour name, via the part object's pid/pindex into basematerials."""
    model = _read(path, "3D/3dmodel.model")
    groups = {g.get("id"): [b.get("name") for b in g.findall("m:base", NS)]
              for g in model.findall("m:resources/m:basematerials", NS)}
    return {o.get("name"): groups[o.get("pid")][int(o.get("pindex"))]
            for o in model.findall("m:resources/m:object", NS) if o.find("m:mesh", NS) is not None}


def test_every_part_has_its_colour(plate):
    colours = _part_colours(plate[0])
    assert colours["flap00_body"] == "black" and colours["flap00_white"] == "white"
    assert colours["flap57_red"] == "red" and colours["flap57_orange"] == "orange"


def test_colours_in_slot_order_with_opaque_display_colour(plate):
    model = _read(plate[0], "3D/3dmodel.model")
    (group,) = model.findall("m:resources/m:basematerials", NS)
    bases = group.findall("m:base", NS)
    assert [COLOUR_SLOTS[b.get("name")] for b in bases] == sorted(COLOUR_SLOTS[b.get("name")] for b in bases)
    assert all(re.fullmatch(r"#[0-9A-F]{6}FF", b.get("displaycolor")) for b in bases)


def test_resource_ids_unique(plate):
    model = _read(plate[0], "3D/3dmodel.model")
    ids = [r.get("id") for r in model.find("m:resources", NS)]
    assert len(ids) == len(set(ids))


def test_assemblies_are_build_items(plate):
    model = _read(plate[0], "3D/3dmodel.model")
    parents = [o.get("id") for o in model.findall("m:resources/m:object", NS)
               if o.find("m:components", NS) is not None]
    items = [i.get("objectid") for i in model.findall("m:build/m:item", NS)]
    assert sorted(items) == sorted(parents)


def test_no_bambu_metadata(plate):
    # A "BambuStudio-" stamp without a full project_settings.config makes Studio
    # load geometry only; a third-party 3mf keeps parts and colours.
    with zipfile.ZipFile(plate[0]) as z:
        assert not any(n.startswith("Metadata/") for n in z.namelist())
        assert b"BambuStudio" not in z.read("3D/3dmodel.model")


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
