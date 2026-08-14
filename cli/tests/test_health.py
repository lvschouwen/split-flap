import json
import pathlib

from splitflap_client.models import StatusAggregate
from splitflap_tui.health import health_sections

FIXDIR = pathlib.Path(__file__).parent / "fixtures"
LIVE = json.loads((FIXDIR / "status_esp32s3.json").read_text())


def sections(payload) -> dict[str, dict[str, str]]:
    return {title: dict(rows)
            for title, rows in health_sections(StatusAggregate.from_json(payload))}


def test_image_section_reads_the_ota_block():
    image = sections(LIVE)["image"]
    assert image["running slot"] == "app1"
    assert image["next slot"] == "app0"
    assert image["ota reverted"] == "no"
    assert image["factory valid"] == "yes"


def test_absent_ota_strings_render_as_dash_not_zero_or_none():
    """Health keys are validity-gated firmware-side, so absent != zero. The
    fixture's lastInvalid is null and lastFlashResult is "" — both must read
    as "-", never "None", "0" or blank."""
    image = sections(LIVE)["image"]
    assert image["last invalid"] == "-"
    assert image["last flash"] == "-"


def test_rescue_and_boot_sections():
    s = sections(LIVE)
    assert s["rescue"]["rescue slot"] == "ok"
    assert s["rescue"]["rescue warn"] == "no"
    assert s["boot"]["reset cause"] == "Brownout"


def test_task_stacks_are_listed_with_the_smallest_flagged():
    """#437 was a clusterTask stack exhaustion — the point of this section is
    seeing the next one coming, so the tightest task must be obvious. hwm is
    BYTES STILL FREE at the worst point since boot, so smallest = closest to
    overflow."""
    tasks = sections(LIVE)["tasks"]
    assert tasks["display"].startswith("12268")
    # mqtt is the tightest in the fixture (1768 B free)
    assert "◂ lowest" in tasks["mqtt"]
    assert sum("◂ lowest" in v for v in tasks.values()) == 1


def test_system_section_converts_units():
    sysm = sections(LIVE)["system"]
    assert sysm["temp"] == "32.6 °C"        # wire is °C x10
    assert sysm["cpu"] == "3% / 1%"         # core0 / core1 load percent
    assert sysm["rssi"] == "-54 dBm"
    assert sysm["ntp age"] == "1h 57m"      # 7037 s since last sync
    # maxAlloc vs heap is the fragmentation read that matters before an OTA
    assert sysm["largest block"].startswith("69")
    assert sysm["psram"].startswith("8")


def test_never_synced_ntp_reads_never_not_minus_one():
    payload = json.loads(json.dumps(LIVE))
    payload["stats"]["now"]["ntpAge"] = -1
    assert sections(payload)["system"]["ntp age"] == "never"


def test_empty_status_renders_dashes_and_never_raises():
    """A board that answers with a truncated/older /status must degrade to
    dashes, not crash the screen."""
    s = sections({})
    assert s["image"]["running slot"] == "-"
    assert s["rescue"]["rescue slot"] == "-"
    assert s["boot"]["reset cause"] == "-"
    assert s["tasks"] == {}                 # no hwm block at all
    for title, rows in s.items():
        assert all(isinstance(v, str) and v for v in rows.values()), title


def test_section_order_is_stable():
    assert [t for t, _ in health_sections(StatusAggregate.from_json(LIVE))] == \
        ["image", "rescue", "boot", "tasks", "system", "config"]


# ---- #473: per-unit detail ------------------------------------------
from splitflap_client.models import UnitsHealth
from splitflap_tui.health import UNIT_DETAIL_KEYS, unit_detail_sections

UNITS = UnitsHealth.from_json(
    json.loads((FIXDIR / "units_health_esp32s3.json").read_text()))


def unit_sections(entry, median=18):
    return {t: dict(rows) for t, rows in unit_detail_sections(entry, median)}


def test_unit_detail_shows_lifetime_and_since_boot_wear_side_by_side():
    """The distinction that actually decides whether a unit needs hands:
    sxl is the LIFETIME worst step-excess, sx forgets at reboot. A unit
    with sxl 1465 and sx 0 misbehaved historically and has been clean this
    boot — the units table alone (lifetime only) cannot say that."""
    u = next(u for u in UNITS.units if u.address == 15)
    wear = unit_sections(u)["wear"]
    assert wear["worst step-excess"] == "1465 lifetime · 0 since boot"


def test_unit_detail_pairs_first_and_latest_self_test():
    """stw0/str0 are the unit's FIRST self-test, stw1/str1 its most recent —
    the unit keeps its own baseline, so the pair reads as a drift check."""
    u = next(u for u in UNITS.units if u.address == 1)
    st = unit_sections(u)["self-test"]
    assert st["hall window"] == "60 → 59"
    assert st["steps/rev"] == "2050 → 2052"


def test_unit_detail_absent_keys_render_dash_not_zero():
    """err/errAge/misses/hf are emit-when-nonzero (UnitHealth.h:451-458), so
    absent means no errors ever charged — never a reading of 0."""
    u = next(u for u in UNITS.units if u.address == 1)
    bus = unit_sections(u)["bus"]
    assert bus["i2c errors"] == "-"
    assert bus["missed heartbeats"] == "-"


def test_unit_detail_reports_a_charged_error_count():
    u = next(u for u in UNITS.units if u.address == 1)
    noisy = type(u).from_json({**u.raw, "err": 4, "errAge": 9000, "misses": 2})
    bus = unit_sections(noisy)["bus"]
    assert bus["i2c errors"].startswith("4")
    assert bus["missed heartbeats"] == "2"


def test_unit_detail_shows_the_row_median_for_context():
    u = next(u for u in UNITS.units if u.address == 6)
    assert "median 18" in unit_sections(u, median=18)["wear"]["row median"]


def test_every_displayed_unit_key_is_documented_by_the_firmware():
    """Drift gate: the labels here are OUR words for the firmware's keys, so
    every key displayed must still exist in the board's own /api legend. If
    firmware renames or drops one, this fails instead of the screen quietly
    showing a dash forever.

    err/errAge are the documented exception — the firmware emits them
    (UnitHealth.h:457) but ApiIndex.h's legend omits them."""
    legend = json.loads((FIXDIR / "api_esp32s3.json").read_text())["legend"]
    undocumented = {"err", "errAge"}
    missing = [k for k in UNIT_DETAIL_KEYS if k not in legend and k not in undocumented]
    assert missing == [], f"keys not in the firmware's /api legend: {missing}"
