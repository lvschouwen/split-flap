"""SSE /events consumer (S3 only; single event name 'display'). One
connection per call — the TUI poller owns reconnect/backoff."""
from __future__ import annotations

import json
from dataclasses import dataclass
from typing import Iterator

import httpx

from .transport import BoardClient, HttpError, Unreachable


@dataclass(frozen=True)
class DisplayEvent:
    text: str
    self_row: int | None      # only while the board leads a wall
    rows: list[str] | None


def _parse(data: str) -> DisplayEvent | None:
    try:
        d = json.loads(data)
    except ValueError:
        return None
    if not isinstance(d, dict) or not isinstance(d.get("text"), str):
        return None
    rows = d.get("rows")
    return DisplayEvent(
        text=d["text"],
        self_row=d["selfRow"] if isinstance(d.get("selfRow"), int) else None,
        rows=[str(r) for r in rows] if isinstance(rows, list) else None)


def display_events(client: BoardClient) -> Iterator[DisplayEvent]:
    try:
        with client.stream("/events") as resp:
            if resp.status_code >= 400:
                raise HttpError(resp.status_code,
                                resp.read().decode(errors="replace"),
                                f"{client.base_url}/events")
            event_name, data_lines = "", []
            for line in resp.iter_lines():
                if line == "":
                    if event_name == "display" and data_lines:
                        parsed = _parse("\n".join(data_lines))
                        if parsed is not None:
                            yield parsed
                    event_name, data_lines = "", []
                elif line.startswith("event:"):
                    event_name = line[6:].strip()
                elif line.startswith("data:"):
                    data_lines.append(line[5:].strip())
    except httpx.TransportError as exc:
        raise Unreachable(f"{client.base_url}/events", exc) from exc
