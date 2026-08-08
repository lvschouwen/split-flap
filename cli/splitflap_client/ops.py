"""The {"seq":N} op contract: submit, then poll /unit/op-result. Single result
slot on the firmware side, last-op-wins — expired means the slot moved on.

Query-param names (WebMaintenance.cpp:305-334):
  /unit/set-address: "address" (source) + "value" (target)
    Line 311: if (!maintRequireLongParam(request, "value", target)) return;
  /unit/clear-address: "address" (source only)
    Line 331: if (!maintCheckAddress(request, snap, addr)) return;
"""
from __future__ import annotations

import time
from dataclasses import dataclass
from typing import Callable

from .transport import BoardClient, ParseError


@dataclass(frozen=True)
class OpResult:
    state: str                       # pending | expired | ok | failed
    reason: str | None = None
    detail: str | None = None


@dataclass(frozen=True)
class SelfTestResult:
    state: str
    reason: str | None
    unit_reason: str | None
    steps_per_rev: int
    hall_window: int
    rev_time_ms: int


def parse_op_result(d: dict) -> OpResult:
    return OpResult(state=str(d.get("state", "")),
                    reason=d.get("reason"), detail=d.get("detail"))


def parse_self_test_result(d: dict) -> SelfTestResult:
    return SelfTestResult(state=str(d.get("state", "")),
                          reason=d.get("reason"),
                          unit_reason=d.get("unit_reason"),
                          steps_per_rev=int(d.get("steps_per_rev", 0) or 0),
                          hall_window=int(d.get("hall_window", 0) or 0),
                          rev_time_ms=int(d.get("rev_time_ms", 0) or 0))


def submit_op(client: BoardClient, path: str, params: dict) -> int:
    resp = client.post(path, params={k: str(v) for k, v in params.items()})
    try:
        return int(resp.json()["seq"])
    except (ValueError, KeyError, TypeError) as exc:
        raise ParseError(f"no seq in op reply from {path}: {resp.text!r}") from exc


def wait_op(client: BoardClient, seq: int, *,
            result_path: str = "/unit/op-result",
            timeout_s: float = 30.0, poll_s: float = 0.5,
            sleep: Callable[[float], None] = time.sleep,
            clock: Callable[[], float] = time.monotonic) -> OpResult:
    deadline = clock() + timeout_s
    while True:
        raw = client.get_json(result_path, params={"seq": str(seq)})
        result = parse_op_result(raw if isinstance(raw, dict) else {})
        if result.state != "pending" or clock() >= deadline:
            return result
        sleep(poll_s)


def run_op(client: BoardClient, path: str, params: dict, **wait_kw) -> OpResult:
    return wait_op(client, submit_op(client, path, params), **wait_kw)
