import xml.etree.ElementTree as ET
import zipfile

from flaps.generate import main

NS = {"m": "http://schemas.microsoft.com/3dmanufacturing/core/2015/02"}


def _assemblies(path):
    with zipfile.ZipFile(path) as z:
        model = ET.fromstring(z.read("3D/3dmodel.model"))
    objs = {o.get("id"): o.get("name") for o in model.findall("m:resources/m:object", NS)}
    return [objs[i.get("objectid")] for i in model.findall("m:build/m:item", NS)]


def test_test_plate_smoke(tmp_path, capsys):
    assert main(["--test-plate", "--out", str(tmp_path)]) == 0
    names = _assemblies(tmp_path / "test-plate.3mf")
    assert len(names) == 14
    assert {"flap01_inlay03", "flap01_t07", "flap01_t10", "flap57"} <= set(names)
    assert "test-plate.3mf" in capsys.readouterr().out


def test_full_set_is_one_file_of_64(tmp_path):
    # One file; Studio's auto-arrange spreads it over as many plates as it needs.
    assert main(["--full", "--out", str(tmp_path)]) == 0
    assert [f.name for f in tmp_path.glob("*.3mf")] == ["drum.3mf"]
    assert sorted(_assemblies(tmp_path / "drum.3mf")) == [f"flap{i:02d}" for i in range(64)]


def test_bad_override_exits_2(tmp_path, capsys):
    assert main(["--test-plate", "--out", str(tmp_path), "--set", "inlay_depth=0.6"]) == 2
    assert "inlay_depth" in capsys.readouterr().err
