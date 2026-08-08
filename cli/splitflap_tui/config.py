from __future__ import annotations

import tomllib
from dataclasses import dataclass
from pathlib import Path

DEFAULT_PATH = Path.home() / ".config" / "splitflap" / "config.toml"


@dataclass(frozen=True)
class Board:
    name: str
    url: str


@dataclass(frozen=True)
class Config:
    boards: list[Board]
    default: str
    poll_s: float = 5.0
    log_poll_s: float = 10.0

    def board_url(self, name: str = "") -> str:
        wanted = name or self.default
        for b in self.boards:
            if b.name == wanted:
                return b.url
        return ""


def load_config(path: Path | None = None) -> Config:
    path = path or DEFAULT_PATH
    if not path.exists():
        return Config(boards=[], default="")
    data = tomllib.loads(path.read_text())
    boards = [Board(str(b.get("name", "")), str(b.get("url", "")).rstrip("/"))
              for b in data.get("boards", [])]
    return Config(boards=boards, default=str(data.get("default", "")),
                  poll_s=float(data.get("poll_s", 5.0)),
                  log_poll_s=float(data.get("log_poll_s", 10.0)))
