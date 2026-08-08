import pytest


@pytest.fixture(autouse=True)
def _hermetic_history(tmp_path, monkeypatch):
    """No test may ever touch the operator's real ~/.config/splitflap/history
    (final-review Critical: the suite had been appending kill-tier test
    commands to the real file, recallable by up-arrow in a live session)."""
    import splitflap_tui.history as history
    monkeypatch.setattr(history, "DEFAULT_HISTORY_PATH",
                        tmp_path / "history-hermetic")
