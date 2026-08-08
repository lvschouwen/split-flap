"""Typed views over firmware JSON. Tolerant: absent/mistyped keys -> defaults."""
from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any

from .capability import plat_from_settings


def _int(d: dict, key: str, default: int = 0) -> int:
    v = d.get(key)
    return int(v) if isinstance(v, (int, float)) and not isinstance(v, bool) else default


def _str(d: dict, key: str, default: str = "") -> str:
    v = d.get(key)
    return v if isinstance(v, str) else default


def _bool(d: dict, key: str, default: bool = False) -> bool:
    v = d.get(key)
    return v if isinstance(v, bool) else default


@dataclass(frozen=True)
class Settings:
    raw: dict
    plat: str
    version: str
    effective_device_name: str
    device_mode: str
    device_role: str
    unit_count: int
    cluster_state: str
    cluster_leading: bool
    cluster_row: int
    cluster_leader_name: str
    heap: int
    rssi: int
    up: int
    rescue_slot: str
    rescue_slot_warn: bool
    last_written_text: str
    last_reset_reason: str
    reflash_on_boot: bool

    @classmethod
    def from_json(cls, d: dict) -> "Settings":
        # esp01 says "width"; S3 says "unitCount".
        units = _int(d, "unitCount", _int(d, "width", 0))
        return cls(
            raw=d, plat=plat_from_settings(d), version=_str(d, "version"),
            effective_device_name=_str(d, "effectiveDeviceName"),
            device_mode=_str(d, "deviceMode"), device_role=_str(d, "deviceRole"),
            unit_count=units, cluster_state=_str(d, "clusterState"),
            cluster_leading=_bool(d, "clusterLeading"),
            cluster_row=_int(d, "clusterRow"),
            cluster_leader_name=_str(d, "clusterLeaderName"),
            heap=_int(d, "heap"), rssi=_int(d, "rssi"), up=_int(d, "up"),
            rescue_slot=_str(d, "rescueSlot"),
            rescue_slot_warn=_bool(d, "rescueSlotWarn"),
            last_written_text=_str(d, "lastWrittenText"),
            last_reset_reason=_str(d, "lastResetReason"),
            reflash_on_boot=_bool(d, "reflashOnBoot"),
        )
