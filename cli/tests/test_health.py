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
