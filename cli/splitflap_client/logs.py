"""Log retrieval. S3: LittleFS flash log (plain text). esp01: 2 KB RAM ring
with a monotonic byte cursor (FollowerLog.h) — first line of the body is the
next cursor, the rest is the delta."""
from __future__ import annotations

from dataclasses import dataclass

from .transport import BoardClient, HttpError, ParseError


def fetch_flash_log(client: BoardClient, prev: bool = False) -> str:
    params = {"prev": "1"} if prev else None
    try:
        return client.get_text("/log/flash", params=params)
    except HttpError as exc:
        if exc.status == 404:      # rotation race / no log yet — benign
            return ""
        raise


@dataclass(frozen=True)
class FollowerLogDelta:
    cursor: int
    text: str


def fetch_follower_log(client: BoardClient, after: int = 0) -> FollowerLogDelta:
    body = client.get_text("/log", params={"after": str(after)})
    head, sep, rest = body.partition("\n")
    try:
        cursor = int(head.strip())
    except ValueError as exc:
        raise ParseError(f"bad follower log cursor line: {head!r}") from exc
    return FollowerLogDelta(cursor=cursor, text=rest if sep else "")
