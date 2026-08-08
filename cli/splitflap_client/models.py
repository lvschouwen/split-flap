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


def _dict(v) -> dict:
    return v if isinstance(v, dict) else {}


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
            wear_flagged=[int(x) for x in flagged if isinstance(x, (int, float)) and not isinstance(x, bool)] if isinstance(flagged, list) else [],
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


@dataclass(frozen=True)
class ClusterMember:
    raw: dict
    host: str
    self_row: bool
    row: int
    col: int
    width: int
    joined: bool
    degraded: bool
    failures: int
    rev: str
    plat: str
    role: str
    suspect: bool
    rescue: bool
    render_stuck: bool
    updating: bool
    update_blocked: bool
    hmac: bool
    faulty: int | None      # None = healthValid block absent

    @classmethod
    def from_json(cls, d: dict) -> "ClusterMember":
        return cls(raw=d, host=_str(d, "host"), self_row=_bool(d, "self"),
                   row=_int(d, "row"), col=_int(d, "col"), width=_int(d, "width"),
                   joined=_bool(d, "joined"), degraded=_bool(d, "degraded"),
                   failures=_int(d, "failures"), rev=_str(d, "rev"),
                   plat=_str(d, "plat", "esp32s3") or "esp32s3",
                   role=_str(d, "role"), suspect=_bool(d, "suspect"),
                   rescue=_bool(d, "rescue"),
                   render_stuck=_bool(d, "renderStuck"),
                   updating=_bool(d, "updating"),
                   update_blocked=_bool(d, "updateBlocked"),
                   hmac=_bool(d, "hmac"), faulty=_opt(d, "faulty"))


@dataclass(frozen=True)
class ClusterStatus:
    raw: dict
    enabled: bool
    epoch: int
    seq: int
    members: list[ClusterMember]
    rollout_phase: str
    rollout_src: str
    follower_image_present: bool
    follower_image_rev: str

    @classmethod
    def from_json(cls, d: dict) -> "ClusterStatus":
        rollout = d.get("rollout") if isinstance(d.get("rollout"), dict) else {}
        fimg = d.get("followerImage") if isinstance(d.get("followerImage"), dict) else {}
        members = d.get("members")
        return cls(raw=d, enabled=_bool(d, "enabled"), epoch=_int(d, "epoch"),
                   seq=_int(d, "seq"),
                   members=[ClusterMember.from_json(m) for m in members
                            if isinstance(m, dict)] if isinstance(members, list) else [],
                   rollout_phase=_str(rollout, "phase"),
                   rollout_src=_str(rollout, "src"),
                   follower_image_present=_bool(fimg, "present"),
                   follower_image_rev=_str(fimg, "rev"))


@dataclass(frozen=True)
class ClusterHealth:
    raw: dict
    state: str
    leader_name: str
    leader_host: str
    row: int
    rev: str
    hmac: bool
    foreign_joins: int
    foreign_pings: int
    foreign_renders: int
    foreign_last_host: str
    stack_free: int | None       # esp01 #435 cont-stack low-water; None on S3

    @classmethod
    def from_json(cls, d: dict) -> "ClusterHealth":
        f = d.get("foreign") if isinstance(d.get("foreign"), dict) else {}
        return cls(raw=d, state=_str(d, "state"),
                   leader_name=_str(d, "leaderName"),
                   leader_host=_str(d, "leaderHost"), row=_int(d, "row"),
                   rev=_str(d, "rev"), hmac=_bool(d, "hmac"),
                   foreign_joins=_int(f, "joins"), foreign_pings=_int(f, "pings"),
                   foreign_renders=_int(f, "renders"),
                   foreign_last_host=_str(f, "lastHost"),
                   stack_free=_opt(d, "stackFree"))


@dataclass(frozen=True)
class SystemStatsNow:
    raw: dict
    rssi: int
    heap: int
    min_heap: int
    cpu0: int
    cpu1: int
    temp_dc: int
    uptime: int
    i2c_tx: int
    i2c_err: int
    ntp_age: int
    reset: str
    hwm: dict[str, int]

    @classmethod
    def from_json(cls, d: dict) -> "SystemStatsNow":
        hwm = d.get("hwm") if isinstance(d.get("hwm"), dict) else {}
        return cls(raw=d, rssi=_int(d, "rssi"), heap=_int(d, "heap"),
                   min_heap=_int(d, "minHeap"), cpu0=_int(d, "cpu0"),
                   cpu1=_int(d, "cpu1"), temp_dc=_int(d, "temp"),
                   uptime=_int(d, "uptime"), i2c_tx=_int(d, "i2cTx"),
                   i2c_err=_int(d, "i2cErr"), ntp_age=_int(d, "ntpAge", -1),
                   reset=_str(d, "reset"),
                   hwm={k: int(v) for k, v in hwm.items()
                        if isinstance(v, (int, float)) and not isinstance(v, bool)})


@dataclass(frozen=True)
class OtaDebug:
    raw: dict
    running: str
    next: str
    last_invalid: str | None
    last_flash_result: str
    ota_reverted: bool
    factory_valid: bool

    @classmethod
    def from_json(cls, d: dict) -> "OtaDebug":
        li = d.get("lastInvalid")
        return cls(raw=d, running=_str(d, "running"), next=_str(d, "next"),
                   last_invalid=li if isinstance(li, str) else None,
                   last_flash_result=_str(d, "lastFlashResult"),
                   ota_reverted=_bool(d, "otaReverted"),
                   factory_valid=_bool(d, "factoryValid"))


@dataclass(frozen=True)
class StatusAggregate:
    raw: dict
    settings: Settings
    stats_now: SystemStatsNow
    units: UnitsHealth
    cluster: ClusterStatus
    ota: OtaDebug

    @classmethod
    def from_json(cls, d: dict) -> "StatusAggregate":
        stats = _dict(d.get("stats"))
        return cls(
            raw=d,
            settings=Settings.from_json(_dict(d.get("settings"))),
            stats_now=SystemStatsNow.from_json(_dict(stats.get("now"))),
            units=UnitsHealth.from_json(_dict(d.get("units"))),
            cluster=ClusterStatus.from_json(_dict(d.get("cluster"))),
            ota=OtaDebug.from_json(_dict(d.get("ota"))),
        )
