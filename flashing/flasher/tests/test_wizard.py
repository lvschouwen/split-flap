from flasher.wizard import network_verdict


def settings(rev="abc1234", detected=3, addrs=(1, 2, 3), width=3):
    return {"version": rev, "detectedUnitCount": detected,
            "detectedUnitAddresses": list(addrs), "unitCount": width}


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


def test_rev_mismatch_reported():
    ok, problems = network_verdict(settings(rev="dead999"), 3, "abc1234")
    assert not ok
    assert any("abc1234" in p and "dead999" in p for p in problems)


def test_unreachable():
    ok, problems = network_verdict(None, 3, "abc1234")
    assert not ok and any("unreachable" in p for p in problems)
