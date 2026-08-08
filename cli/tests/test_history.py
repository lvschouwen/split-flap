from splitflap_tui.history import load_history, save_history


def test_roundtrip_and_cap(tmp_path):
    p = tmp_path / "history"
    save_history([f"cmd {i}" for i in range(150)], p)
    got = load_history(p)
    assert len(got) == 100
    assert got[-1] == "cmd 149"


def test_load_missing_file_returns_empty(tmp_path):
    assert load_history(tmp_path / "nope") == []


def test_save_into_unwritable_parent_is_silent(tmp_path):
    blocker = tmp_path / "file"
    blocker.write_text("x")                    # a FILE where a dir is needed
    save_history(["a"], blocker / "history")    # must not raise
    assert load_history(blocker / "history") == []
