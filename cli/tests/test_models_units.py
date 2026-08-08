from splitflap_client.models import UnitsHealth

FULL = {"width": 2, "faulty": 1, "vccMin": 4780,
        "units": [
            {"i": 0, "a": 1, "st": 1, "v": 1, "rev": "d6e8a8a", "odo": 1257,
             "sx": 17, "sxl": 37, "vcc": 4900, "vmin": 4780, "gates": 0,
             "age": 412},
            {"i": 1, "a": 15, "st": 1, "v": 1, "rev": "d6e8a8a", "sx": 1465,
             "stale": 1, "hf": 3},
        ],
        "wear": {"median": 1200, "flagged": [1]},
        "reflash": {"state": "idle", "total": 0, "done": 0, "failed": 0,
                    "cur": 0, "halted": False}}


def test_full_parse():
    h = UnitsHealth.from_json(FULL)
    assert h.width == 2 and h.faulty == 1 and h.vcc_min == 4780
    assert h.wear_flagged == [1] and h.reflash_state == "idle"
    u0, u1 = h.units
    assert u0.sx == 17 and u0.odo == 1257 and not u0.stale and not u0.fault
    assert u1.sx == 1465 and u1.stale and u1.fault and u1.hall_fails == 3
    assert u1.odo is None and u1.err is None      # absent keys stay None


def test_status_aggregate_variant_without_wear_reflash():
    # /status units section lacks the wear+reflash splices (WebSystem.cpp:325)
    d = {"width": 1, "faulty": 0, "units": [{"i": 0, "a": 1, "st": 0, "v": 0}]}
    h = UnitsHealth.from_json(d)
    assert h.wear_flagged == [] and h.reflash_state == "" and h.vcc_min is None
    assert h.units[0].fault        # st=0 (silent) counts as fault for display


def test_overflow_fallback_shape():
    h = UnitsHealth.from_json({"width": 16, "faulty": 2, "units": []})
    assert h.width == 16 and h.units == []
