import xml.etree.ElementTree as ET
import zipfile

import pytest

from drum.generate import main

NS = {"m": "http://schemas.microsoft.com/3dmanufacturing/core/2015/02"}


def _names(path):
    with zipfile.ZipFile(path) as z:
        model = ET.fromstring(z.read("3D/3dmodel.model"))
    objs = {o.get("id"): o.get("name") for o in model.findall("m:resources/m:object", NS)}
    return [objs[i.get("objectid")] for i in model.findall("m:build/m:item", NS)]


def test_test_assembly_smoke(tmp_path, capsys):
    assert main(["--test-assembly", "--out", str(tmp_path)]) == 0
    names = []
    for f in sorted(tmp_path.glob("test-assembly-*.3mf")):
        names += _names(f)
    assert sorted(names) == sorted(["drum-a", "drum-b", "wall-motor", "wall-far", "bearing-ring", "rail-bottom",
                                    "rail-top", "front-plate", "back-plate", "stop-lip"])
    assert "test-assembly-1.3mf" in capsys.readouterr().out


def test_full_adds_the_sleeve_and_coupon_has_eight_blocks(tmp_path):
    assert main(["--full", "--coupon", "--out", str(tmp_path)]) == 0
    assert "sleeve" in sum((_names(f) for f in tmp_path.glob("full-*.3mf")), [])
    assert len(_names(tmp_path / "coupon-1.3mf")) == 8


def test_override_and_bad_override(tmp_path):
    assert main(["--coupon", "--set", "hole_slop=0.1", "--out", str(tmp_path)]) == 0
    with pytest.raises(ValueError):
        main(["--coupon", "--set", "r_p=10", "--out", str(tmp_path)])
