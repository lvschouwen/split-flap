import httpx
import pytest
from splitflap_client.ops import (OpResult, parse_op_result,
                                  parse_self_test_result, run_op, submit_op,
                                  wait_op)
from splitflap_client.transport import BoardClient, HttpError


def make_client(handler):
    return BoardClient("http://b", transport=httpx.MockTransport(handler))


def test_parse_op_result_all_shapes():
    assert parse_op_result({"state": "pending"}) == OpResult("pending")
    assert parse_op_result({"state": "ok"}) == OpResult("ok")
    assert parse_op_result({"state": "expired"}) == OpResult("expired")
    r = parse_op_result({"state": "failed", "reason": "postcondition-fail",
                         "detail": "unit-missing-after-reprobe"})
    assert r.state == "failed" and r.detail == "unit-missing-after-reprobe"


def test_submit_returns_seq_and_sends_query_params():
    def handler(req):
        assert req.url.params["address"] == "3"
        return httpx.Response(200, json={"seq": 41})
    assert submit_op(make_client(handler), "/unit/home", {"address": 3}) == 41


def test_submit_409_propagates_verbatim():
    body = "Unit reflash in progress — retry when it finishes"
    c = make_client(lambda r: httpx.Response(409, text=body))
    with pytest.raises(HttpError) as exc:
        submit_op(c, "/unit/home", {"address": 3})
    assert exc.value.body == body


def test_wait_op_polls_until_ok():
    answers = [{"state": "pending"}, {"state": "pending"}, {"state": "ok"}]
    def handler(req):
        assert req.url.params["seq"] == "41"
        return httpx.Response(200, json=answers.pop(0))
    naps = []
    r = wait_op(make_client(handler), 41, sleep=naps.append,
                clock=lambda: len(naps) * 0.5)
    assert r == OpResult("ok") and len(naps) == 2


def test_wait_op_timeout_returns_pending():
    c = make_client(lambda r: httpx.Response(200, json={"state": "pending"}))
    ticks = iter([0, 1, 2, 31, 32])
    r = wait_op(c, 7, timeout_s=30.0, sleep=lambda s: None,
                clock=lambda: next(ticks))
    assert r == OpResult("pending")


def test_self_test_result_parse():
    ok = parse_self_test_result({"state": "ok", "steps_per_rev": 2038,
                                 "hall_window": 46, "rev_time_ms": 1200})
    assert ok.steps_per_rev == 2038 and ok.state == "ok"
    bad = parse_self_test_result({"state": "failed", "reason": "unit-failed",
                                  "unit_reason": "hall-stuck",
                                  "steps_per_rev": 0, "hall_window": 0,
                                  "rev_time_ms": 0})
    assert bad.unit_reason == "hall-stuck"


def test_run_op_end_to_end():
    calls = {"n": 0}
    def handler(req):
        if req.url.path == "/unit/identify":
            return httpx.Response(200, json={"seq": 5})
        calls["n"] += 1
        state = {"state": "pending"} if calls["n"] < 2 else {"state": "ok"}
        return httpx.Response(200, json=state)
    r = run_op(make_client(handler), "/unit/identify", {"address": 2},
               sleep=lambda s: None, clock=lambda: calls["n"] * 0.1)
    assert r.state == "ok"
