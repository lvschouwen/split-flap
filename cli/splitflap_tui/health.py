"""Pure board-health rendering (#472). No I/O, no Textual.

Everything here comes off the `/status` aggregate the dashboard already
polls — this module only decides what to show and how to word it, so the
screen stays a renderer and the wording is testable.

Two rules the firmware forces on us:

- **Absent is not zero.** Health keys are validity-gated firmware-side, so a
  missing value means "no reading", not "reading of 0". Everything absent
  renders "-".
- **Units come from the wire's own legend** (Master/ApiIndex.h): `temp` is
  °C x10, `cpu0`/`cpu1` are load percent, `ntpAge` is seconds with -1 for
  never, and `hwm` values are BYTES STILL FREE at each task's worst point
  since boot (SystemStatsPolicy.h:85) — so the SMALLEST is the closest to
  overflow, which is what #437 hit.
"""
from __future__ import annotations

from splitflap_client.models import StatusAggregate

from .format import human_duration, human_size

Section = tuple[str, list[tuple[str, str]]]

LOWEST_MARK = "◂ lowest"


def _dash(value) -> str:
    """Absent (None) and empty-string both read as no-reading."""
    return "-" if value is None or value == "" else str(value)


def _yes_no(value: bool | None) -> str:
    return "-" if value is None else ("yes" if value else "no")


def _bytes(n: int | None) -> str:
    # human_size already carries the k/M prefix, so no space before the B.
    return "-" if n is None else f"{human_size(n)}B"


def _ntp_age(seconds: int | None) -> str:
    # -1 is the firmware's "never synced" sentinel, not a duration.
    if seconds is None or seconds < 0:
        return "never"
    return human_duration(seconds)


def _tasks(hwm: dict[str, int]) -> list[tuple[str, str]]:
    if not hwm:
        return []
    lowest = min(hwm.values())
    rows = []
    for name, free in sorted(hwm.items(), key=lambda kv: kv[1]):
        mark = f"  {LOWEST_MARK}" if free == lowest else ""
        rows.append((name, f"{free} B free{mark}"))
    return rows


def health_sections(agg: StatusAggregate) -> list[Section]:
    s, st, ota = agg.settings, agg.stats_now, agg.ota
    return [
        ("image", [
            ("rev", _dash(s.version)),
            ("running slot", _dash(ota.running)),
            ("next slot", _dash(ota.next)),
            ("last flash", _dash(ota.last_flash_result)),
            ("last invalid", _dash(ota.last_invalid)),
            ("ota reverted", _yes_no(ota.ota_reverted)),
            ("factory valid", _yes_no(ota.factory_valid)),
        ]),
        ("rescue", [
            ("rescue slot", _dash(s.rescue_slot)),
            ("rescue warn", _yes_no(s.rescue_slot_warn)),
        ]),
        ("boot", [
            ("reset cause", _dash(s.last_reset_reason or st.reset)),
            ("uptime", human_duration(st.uptime or s.up)),
        ]),
        ("tasks", _tasks(st.hwm)),
        ("system", [
            ("heap", _bytes(st.heap)),
            ("min heap", _bytes(st.min_heap)),
            ("largest block", _bytes(st.max_alloc)),
            ("psram", _bytes(st.psram)),
            ("cpu", f"{st.cpu0}% / {st.cpu1}%"),
            ("temp", f"{st.temp_dc / 10:.1f} °C"),
            ("rssi", f"{st.rssi} dBm"),
            ("ntp age", _ntp_age(st.ntp_age)),
            ("i2c tx/err", f"{st.i2c_tx} / {st.i2c_err}"),
            ("mqtt drops", str(st.mqtt_drops)),
        ]),
        ("config", [
            ("role", _dash(s.device_role)),
            ("mode", _dash(s.device_mode)),
            ("reflash on boot", _yes_no(s.reflash_on_boot)),
            ("cluster leader", _dash(s.cluster_leader_name)),
        ]),
    ]
