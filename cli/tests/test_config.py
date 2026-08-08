from pathlib import Path

from splitflap_tui.config import Board, load_config


def test_load_config(tmp_path: Path):
    p = tmp_path / "config.toml"
    p.write_text('default = "leader"\n'
                 '[[boards]]\nname = "leader"\nurl = "http://10.0.0.2"\n'
                 '[[boards]]\nname = "row0"\nurl = "http://10.0.0.3"\n')
    cfg = load_config(p)
    assert cfg.default == "leader"
    assert cfg.boards[0] == Board("leader", "http://10.0.0.2")
    assert cfg.poll_s == 5.0


def test_missing_file_is_empty_config(tmp_path: Path):
    cfg = load_config(tmp_path / "nope.toml")
    assert cfg.boards == [] and cfg.default == ""
