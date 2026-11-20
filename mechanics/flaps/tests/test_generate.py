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
    assert len(names) == 13
    assert {"flap01_inlay03", "flap01_t12", "flap57"} <= set(names)
    assert "test-plate.3mf" in capsys.readouterr().out


def test_full_set_covers_64(tmp_path):
    assert main(["--full", "--out", str(tmp_path)]) == 0
    names = []
    for f in sorted(tmp_path.glob("drum-plate-*.3mf")):
        names += _assemblies(f)
    assert sorted(names) == [f"flap{i:02d}" for i in range(64)]


def test_bad_override_exits_2(tmp_path, capsys):
    assert main(["--test-plate", "--out", str(tmp_path), "--set", "inlay_depth=0.6"]) == 2
    assert "inlay_depth" in capsys.readouterr().err


def test_full_set_fills_two_plates(tmp_path):
    # Plain flaps fill whole plates; the plain remainder shares the tower plate with the colour flaps.
    main(["--full", "--out", str(tmp_path)])
    plates = sorted(tmp_path.glob("drum-plate-*.3mf"))
    assert [len(_assemblies(f)) for f in plates] == [42, 22]
    assert {"flap57", "flap63"} <= set(_assemblies(plates[1]))
