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


def _opt(d: dict, key: str) -> int | None:
    v = d.get(key)
    return int(v) if isinstance(v, (int, float)) and not isinstance(v, bool) else None


@dataclass(frozen=True)
class UnitEntry:
    raw: dict
    index: int
    address: int
    state: int          # 0 silent / 1 sketch / 2 bootloader
    valid: bool
    rev: str

    @classmethod
    def from_json(cls, d: dict) -> "UnitEntry":
        return cls(raw=d, index=_int(d, "i"), address=_int(d, "a"),
                   state=_int(d, "st"), valid=_int(d, "v") == 1,
                   rev=_str(d, "rev"))

    @property
    def stale(self) -> bool:
        return d.get("stale") == 1 if (d := self.raw) else False

    @property
    def fault(self) -> bool:
        return self.state != 1 or self.stale

    # Absent = None (key emission is validity-gated, UnitHealth.h:295-412).
    @property
    def sx(self): return _opt(self.raw, "sx")
    @property
    def sxl(self): return _opt(self.raw, "sxl")
    @property
    def odo(self): return _opt(self.raw, "odo")
    @property
    def vcc_min(self): return _opt(self.raw, "vmin")
    @property
    def gates(self): return _opt(self.raw, "gates")
    @property
    def hall_fails(self): return _opt(self.raw, "hf")
    @property
    def err(self): return _opt(self.raw, "err")
    @property
    def err_age(self): return _opt(self.raw, "errAge")
    @property
    def age(self): return _opt(self.raw, "age")
    @property
    def misses(self): return _opt(self.raw, "misses")


@dataclass(frozen=True)
class UnitsHealth:
    raw: dict
    width: int
    faulty: int
    vcc_min: int | None
    units: list[UnitEntry]
    wear_flagged: list[int]
    reflash_state: str
    reflash_halted: bool

    @classmethod
    def from_json(cls, d: dict) -> "UnitsHealth":
        wear = d.get("wear") if isinstance(d.get("wear"), dict) else {}
        reflash = d.get("reflash") if isinstance(d.get("reflash"), dict) else {}
        flagged = wear.get("flagged")
        units_raw = d.get("units")
        return cls(
            raw=d, width=_int(d, "width"), faulty=_int(d, "faulty"),
            vcc_min=_opt(d, "vccMin"),
            units=[UnitEntry.from_json(u) for u in units_raw
                   if isinstance(u, dict)] if isinstance(units_raw, list) else [],
            wear_flagged=[int(x) for x in flagged] if isinstance(flagged, list) else [],
            reflash_state=_str(reflash, "state"),
            reflash_halted=_bool(reflash, "halted"),
        )


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
