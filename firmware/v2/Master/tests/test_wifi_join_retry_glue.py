"""Source gate for the leader's WiFi join-retry glue (#524).

WifiPolicy.h decides (natively tested); WifiService.cpp has to feed it the
tally from reset-surviving memory and write that tally back at the right
moments. None of that is reachable from a native test, and getting one of the
writes wrong either loops a board through reboots forever or sends it to the
portal after a single failed join.
"""
import re
from pathlib import Path

SRC = Path(__file__).resolve().parent.parent / "WifiService.cpp"


def _code():
    return re.sub(r"//[^\n]*", "", SRC.read_text())


def _body(code, signature):
    start = code.index(signature)
    i = code.index("{", start)
    depth = 0
    for j in range(i, len(code)):
        depth += {"{": 1, "}": -1}.get(code[j], 0)
        if depth == 0:
            return code[i + 1:j]
    raise AssertionError(signature)


def test_tally_lives_in_reset_surviving_memory():
    assert "RTC_NOINIT_ATTR static WifiJoinRetryRecord joinRetry;" in _code()


def test_policy_is_seeded_from_the_tally_before_it_first_steps():
    init = _body(_code(), "void wifiServiceInit(")
    assert "policy.failedJoinBoots = wifiJoinRetryDecode(joinRetry);" in init


def test_a_retry_records_the_attempt_before_it_restarts():
    tick = _body(_code(), "void wifiServiceTick(")
    case = tick[tick.index("case WifiAction::RetryJoin:"):]
    case = case[:case.index("break;")]
    encode = case.index("wifiJoinRetryEncode(joinRetry, attempt);")
    assert "attempt = (uint8_t)(policy.failedJoinBoots + 1)" in case
    assert encode < case.index("scheduleRestart(")


def test_joining_and_the_portal_both_clear_the_tally():
    code = _code()
    assert "wifiJoinRetryEncode(joinRetry, 0);" in _body(code, "static void startOnline(")
    assert "wifiJoinRetryEncode(joinRetry, 0);" in _body(code, "static void startPortal(")


def test_the_tally_is_written_nowhere_else():
    # Three writes: a retry, joining, the portal. A fourth is a new way to get
    # the count wrong.
    assert len(re.findall(r"wifiJoinRetryEncode\(", _code())) == 3


def test_no_retry_restart_while_a_unit_reflash_runs():
    tick = _body(_code(), "void wifiServiceTick(")
    step = tick.index("wifiPolicyStep(policy,")
    hold = tick.index("!wifiRestartMayProceed(", step)
    assert "reflashInProgress(displaySnapshotGet().reflash)" in tick[hold:hold + 200]
    # Decided before the switch acts on the action.
    assert hold < tick.index("switch (action)", step)


def test_the_retry_reboot_cause_carries_this_windows_disconnect_reason():
    code = _code()
    # Stored by the event handler, cleared when a join window opens, so the
    # reason reported is from the window that failed.
    handler = _body(code, "static void onWifiStaEvent(")
    assert "lastJoinDisconnectReason.store((uint8_t)info.wifi_sta_disconnected.reason" in handler
    assert "lastJoinDisconnectReason.store(0," in _body(code, "static void startJoin(")
    tick = _body(code, "void wifiServiceTick(")
    case = tick[tick.index("case WifiAction::RetryJoin:"):]
    case = case[:case.index("break;")]
    assert "lastJoinDisconnectReason.load(" in case
    assert 'reason == 0 ? String(F("none"))' in case

