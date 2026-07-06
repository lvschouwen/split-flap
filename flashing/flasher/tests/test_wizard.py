from flasher.session import Session
from flasher.wizard import _is_completed_session, network_verdict


def settings(rev="abc1234", detected=3, addrs=(1, 2, 3), width=3, status=None):
    if status is None:
        status = [0] * 16
    return {"version": rev, "detectedUnitCount": detected,
            "detectedUnitAddresses": list(addrs), "unitCount": width,
            "detectedUnitVersionStatus": status}


def test_all_good():
    ok, problems = network_verdict(settings(), 3, "abc1234")
    assert ok and problems == []


def test_uses_detected_count_not_display_width():
    # units 1 and 12 alive -> unitCount(displayWidth)=12 but detected=2. Must fail.
    s = settings(detected=2, addrs=(1, 12), width=12)
    ok, problems = network_verdict(s, 12, "abc1234")
    assert not ok
    assert any("missing" in p for p in problems)


def test_missing_addresses_are_named():
    s = settings(detected=2, addrs=(1, 3), width=3)
    ok, problems = network_verdict(s, 3, "abc1234")
    assert not ok
    assert any("2" in p for p in problems)
    assert not any("not running current firmware" in p for p in problems)


def test_rev_mismatch_reported():
    ok, problems = network_verdict(settings(rev="dead999"), 3, "abc1234")
    assert not ok
    assert any("abc1234" in p and "dead999" in p for p in problems)


def test_unreachable():
    ok, problems = network_verdict(None, 3, "abc1234")
    assert not ok and any("unreachable" in p for p in problems)


# --- FIX 1: bootloader-stuck / stale / unknown units must not read as "good" ---

def test_all_present_all_healthy_status_is_good():
    s = settings(detected=3, addrs=(1, 2, 3), width=3, status=[0] * 16)
    ok, problems = network_verdict(s, 3, "abc1234")
    assert ok and problems == []


def test_unit_stuck_in_bootloader_not_good():
    # address present (twiboot answers I2C) but status still "unknown" (2)
    status = [0] * 16
    status[2] = 2  # unit 3
    s = settings(detected=3, addrs=(1, 2, 3), width=3, status=status)
    ok, problems = network_verdict(s, 3, "abc1234")
    assert not ok
    assert any("3" in p and "not running current firmware" in p for p in problems)
    assert not any("missing" in p for p in problems)


def test_unit_outdated_firmware_not_good():
    status = [0] * 16
    status[2] = 1  # unit 3 outdated
    s = settings(detected=3, addrs=(1, 2, 3), width=3, status=status)
    ok, problems = network_verdict(s, 3, "abc1234")
    assert not ok
    assert any("3" in p and "not running current firmware" in p for p in problems)


def test_missing_address_case_still_uses_missing_message_only():
    s = settings(detected=2, addrs=(1, 3), width=3)  # unit 2 missing entirely
    ok, problems = network_verdict(s, 3, "abc1234")
    assert not ok
    assert any("missing" in p for p in problems)
    assert not any("not running current firmware" in p for p in problems)


def test_short_detected_unit_version_status_array_does_not_crash_and_fails_safe():
    # array shorter than expected units — missing index must be treated unhealthy
    s = settings(detected=3, addrs=(1, 2, 3), width=3, status=[0, 0])
    ok, problems = network_verdict(s, 3, "abc1234")
    assert not ok
    assert any("3" in p and "not running current firmware" in p for p in problems)


def test_missing_detected_unit_version_status_key_fails_safe():
    s = settings(detected=3, addrs=(1, 2, 3), width=3)
    del s["detectedUnitVersionStatus"]
    ok, problems = network_verdict(s, 3, "abc1234")
    assert not ok
    assert any("not running current firmware" in p for p in problems)


# --- FIX 3: a completed session must not be silently reused ---

def test_completed_session_is_recognized_as_complete():
    s = Session(unit_count=3, done=[1, 2, 3])
    assert _is_completed_session(s) is True


def test_completed_session_via_skip_list_is_recognized_as_complete():
    s = Session(unit_count=3, done=[1, 2, 3], skipped=[2])
    assert _is_completed_session(s) is True


def test_in_progress_session_is_not_complete():
    s = Session(unit_count=3, done=[1, 2])
    assert _is_completed_session(s) is False


def test_none_session_is_not_complete():
    assert _is_completed_session(None) is False


def test_fresh_zero_unit_count_session_is_not_complete():
    assert _is_completed_session(Session(unit_count=0)) is False
