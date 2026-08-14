"""Leader-side board discovery (#274): the staged POST/GET /cluster/discover
pair, driven from the host. There is deliberately no client-side mDNS —
multicast does not cross the operator VPN, so the leader does the browsing and
we read its result.

Wire contract (WebCluster.cpp:497-524 + the netTask drain at :635):
  POST /cluster/discover -> 200 "Board discovery started". Arms a staged flag;
    a re-POST while one is pending is a no-op (the flag is its own re-entry
    guard). The blocking MDNS.queryService runs in netTask, never a handler.
  GET  /cluster/discover -> 202 "Discovery running"  while pending
                            200 {"status":"done","boards":[...]}  when done
                            404 "No discovery has run yet"  if none ever ran
The result latches until the next POST, and the board filters its own
advertisement out (capped at CLUSTER_DISCOVER_MAX_BOARDS = 8).
"""
from __future__ import annotations

import time
from dataclasses import dataclass
from typing import Callable

from .capability import PLAT_ESP01, PLAT_S3
from .transport import BoardClient, ParseError, SplitflapError

PATH = "/cluster/discover"

# Same 10 s deadline / 500 ms cadence the Web UI's own Cluster card scan uses
# (data/script.js:3374-3380), so the two clients agree on how long a scan may
# take rather than each inventing a bound.
DEFAULT_TIMEOUT_S = 10.0
DEFAULT_POLL_S = 0.5

_STILL_RUNNING = 202


class DiscoverTimeout(SplitflapError):
    """The leader was still scanning when the deadline passed. Its result
    latches, so a later re-run can still collect it."""


@dataclass(frozen=True)
class DiscoveredBoard:
    name: str      # mDNS/TXT name, no ".local"
    host: str      # dotted quad, or "<name>.local" when the answer carried none
    rev: str       # TXT rev — firmware short-hash, "" when absent
    width: int     # TXT width — 0 when absent/unparseable
    plat: str      # esp32s3 | esp01


def _board(d: dict) -> DiscoveredBoard:
    name = d.get("name")
    host = d.get("host")
    rev = d.get("rev")
    width = d.get("width")
    return DiscoveredBoard(
        name=name if isinstance(name, str) else "",
        host=host if isinstance(host, str) else "",
        rev=rev if isinstance(rev, str) else "",
        # TXT width rides the wire as a JSON number; anything else is unknown.
        width=int(width) if isinstance(width, (int, float))
        and not isinstance(width, bool) else 0,
        # #297: only foreign platforms carry a plat tag, so absent = S3 —
        # the same inference plat_from_settings makes for /settings.
        plat=PLAT_ESP01 if d.get("plat") == PLAT_ESP01 else PLAT_S3,
    )


def parse_discover(payload) -> list[DiscoveredBoard]:
    """Tolerant read of the done payload: absent/mistyped keys fall back to
    defaults and non-dict entries are dropped, so an additive firmware key
    can never break a scan."""
    boards = payload.get("boards") if isinstance(payload, dict) else None
    if not isinstance(boards, list):
        return []
    return [_board(b) for b in boards if isinstance(b, dict)]


def run_discover(client: BoardClient, *,
                 timeout_s: float = DEFAULT_TIMEOUT_S,
                 poll_s: float = DEFAULT_POLL_S,
                 sleep: Callable[[float], None] = time.sleep,
                 clock: Callable[[], float] = time.monotonic
                 ) -> list[DiscoveredBoard]:
    """Arm a scan on the leader, then poll until it reports done. Raises
    DiscoverTimeout past the deadline; HttpError (404/5xx) and ParseError
    propagate verbatim. An empty list means the scan found nothing — across
    the VPN that is the normal answer, not a failure."""
    client.post(PATH)
    deadline = clock() + timeout_s
    while True:
        resp = client.get(PATH)
        if resp.status_code != _STILL_RUNNING:
            try:
                payload = resp.json()
            except ValueError as exc:
                raise ParseError(f"invalid JSON from {resp.url}") from exc
            return parse_discover(payload)
        if clock() >= deadline:
            raise DiscoverTimeout(
                f"discovery still running after {timeout_s:g}s")
        sleep(poll_s)
