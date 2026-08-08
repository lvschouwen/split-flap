# splitflap Operator TUI Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** One interactive Textual program (`splitflap`) that operates and health-checks the wall over the existing firmware HTTP surface — epic #441, children #442–#447, spec `docs/superpowers/specs/2026-08-08-splitflap-tui-design.md`.

**Architecture:** Pure client library `splitflap_client` (transport, typed models, static per-platform capability table, `{"seq":N}` op contract, SSE consumer — no printing) under a Textual app `splitflap_tui` (dashboard, command bar, detail screens — no direct HTTP). Leader-only polling; SSE is the only wall-row truth.

**Tech Stack:** Python 3.11+, `textual`, `httpx`; dev: `pytest`, `pytest-asyncio`. No pydantic — dataclasses + tolerant `from_json` parsers.

## Global Constraints

- Everything lives in `cli/`; nothing outside it changes except `.github/workflows/build.yml` (Task 13) and the CLAUDE.md sync line (Task 13).
- Platform literals are exactly `"esp32s3"` and `"esp01"` (wire values). A missing/empty/unknown `plat` settings key means S3 (`ota-flash.sh` precedent, flashing/ota-flash.sh:491-500).
- Firmware error bodies (409/503/400) are surfaced **verbatim**; mutating requests are **never retried**.
- Display control is `POST /` `application/x-www-form-urlencoded` with `ajax=1` always set (WebSettings.cpp:35-125). There is no `/display/text` and no notify route — `transientText`+`transientDwell` IS the notification.
- Op routes take **query params** (`address`, `value`, `steps`, `gates`; strtol base 0) and return `200 {"seq":N}` (WebMaintenance.cpp:90-101).
- SSE `/events` has exactly one event name, `display`; payload `{"text":…}` plus `selfRow`+`rows` only while leading (DisplayEvents.h:61-78). S3-only.
- Timeouts: connect 2 s, read 5 s (SSE: read unlimited). The TUI never background-polls a follower; follower endpoints are called only from its open board-detail screen at ≥5 s cadence.
- Public repo: review captured fixtures before committing (drop anything private; `mqttPasswordSet` is a bool, never a secret, but blank `mqttHost`/`mqttUser`/`deviceName` values in fixtures before commit).
- Commit messages reference the child issues (#442–#447). Work continues on branch `feat/splitflap-tui-spec`.
- All test commands run from `cli/`: `python -m pytest tests/ -v`.

## File Structure

```
cli/
  pyproject.toml                  # package metadata, deps, entry point `splitflap`
  README.md                       # install + usage (Task 13)
  splitflap_client/
    __init__.py                   # re-exports
    transport.py                  # errors + BoardClient                (Task 1)
    capability.py                 # plat literals, route tables, serves() (Task 2)
    models.py                     # Settings/UnitsHealth/Cluster/Stats/Ota/StatusAggregate (Tasks 3-5)
    logs.py                       # S3 flash log + esp01 ring cursor    (Task 6)
    ops.py                        # seq submit + op/self-test result state machines (Task 7)
    control.py                    # POST / form, stop, reboot           (Task 8)
    events.py                     # SSE display_events generator        (Task 9)
  splitflap_tui/
    __init__.py
    __main__.py                   # python -m splitflap_tui / console script
    config.py                     # ~/.config/splitflap/config.toml     (Task 11)
    app.py                        # SplitflapApp + DashboardScreen      (Task 11)
    poller.py                     # thread workers: poll + SSE loop     (Task 11)
    widgets.py                    # WallPanel/ClusterStrip/UnitsTable/LogTail/StatsBar (Task 11)
    commands.py                   # pure parser + tier table            (Task 12)
    confirm.py                    # ConfirmModal (one-key + typed)      (Task 12)
    screens/__init__.py
    screens/board_detail.py       # per-board screen                    (Task 13)
    screens/log_screen.py         # full log + prev toggle              (Task 13)
  tools/
    capture_fixtures.py           # curl live boards → tests/fixtures/  (Task 2)
  tests/
    fixtures/                     # api_esp32s3.json, api_esp01.json, + optional live captures
    test_transport.py             # Task 1
    test_capability.py            # Task 2
    test_models_settings.py       # Task 3
    test_models_units.py          # Task 4
    test_models_cluster.py        # Task 5
    test_logs.py                  # Task 6
    test_ops.py                   # Task 7
    test_control.py               # Task 8
    test_events.py                # Task 9
    test_wire_twin.py             # Task 10 (integration, auto-skips)
    test_config.py                # Task 11
    test_app.py                   # Task 11 (Pilot)
    test_commands.py              # Task 12
    test_screens.py               # Task 13 (Pilot)
```

Reference for every JSON shape used below: the serializers named per task (verified 2026-08-08). Key ones: `/status` = WebSystem.cpp:318-349, `/settings` = SettingsJson.h:123-201, `/units/health` = shared/UnitHealth.h:280-420, `/cluster/status` = Master/ClusterDigest.h:79-190, op-result = DisplayIpc.h:352-374, follower equivalents FollowerJson.h / FollowerOps.h (byte-identical op vocabularies).

---

### Task 1: Scaffold + transport (`splitflap_client.transport`) — #442

**Files:**
- Create: `cli/pyproject.toml`, `cli/splitflap_client/__init__.py`, `cli/splitflap_client/transport.py`, `cli/splitflap_tui/__init__.py`, `cli/tests/__init__.py` (empty), `cli/tests/test_transport.py`

**Interfaces:**
- Produces: `SplitflapError`, `Unreachable(url, cause)`, `HttpError(status, body, url)`, `ParseError`; `DEFAULT_TIMEOUT`; `BoardClient(base_url, *, timeout=DEFAULT_TIMEOUT, transport=None)` with `.get_json(path, params=None) -> Any`, `.get_text(path, params=None) -> str`, `.post(path, *, params=None, data=None) -> httpx.Response`, `.close()`, context-manager support. Every later task consumes `BoardClient` and these errors. The `transport=` kwarg is the seam every test uses (`httpx.MockTransport`).

- [ ] **Step 1: Write pyproject**

```toml
[project]
name = "splitflap-cli"
version = "0.1.0"
description = "Operator TUI for the split-flap wall"
requires-python = ">=3.11"
dependencies = ["textual>=0.80", "httpx>=0.27"]

[project.optional-dependencies]
dev = ["pytest>=8", "pytest-asyncio>=0.23"]

[project.scripts]
splitflap = "splitflap_tui.__main__:main"

[build-system]
requires = ["setuptools>=68"]
build-backend = "setuptools.build_meta"

[tool.setuptools]
packages = ["splitflap_client", "splitflap_tui", "splitflap_tui.screens"]

[tool.pytest.ini_options]
asyncio_mode = "auto"
```

- [ ] **Step 2: Write the failing tests** (`cli/tests/test_transport.py`)

```python
import httpx
import pytest
from splitflap_client.transport import BoardClient, HttpError, ParseError, Unreachable


def make_client(handler):
    return BoardClient("http://board", transport=httpx.MockTransport(handler))


def test_get_json_parses_body():
    client = make_client(lambda req: httpx.Response(200, json={"plat": "esp32s3"}))
    assert client.get_json("/settings") == {"plat": "esp32s3"}


def test_http_error_carries_verbatim_body_and_status():
    body = "Unit reflash in progress — retry when it finishes"
    client = make_client(lambda req: httpx.Response(409, text=body))
    with pytest.raises(HttpError) as exc:
        client.post("/unit/home", params={"address": 3})
    assert exc.value.status == 409
    assert exc.value.body == body


def test_transport_error_wraps_as_unreachable():
    def handler(req):
        raise httpx.ConnectError("refused")
    client = make_client(handler)
    with pytest.raises(Unreachable):
        client.get_json("/settings")


def test_invalid_json_is_parse_error():
    client = make_client(lambda req: httpx.Response(200, text="not json"))
    with pytest.raises(ParseError):
        client.get_json("/settings")


def test_post_sends_form_data():
    seen = {}
    def handler(req):
        seen["content"] = req.content.decode()
        return httpx.Response(200, text="ok")
    client = make_client(handler)
    client.post("/", data={"inputText": "HELLO", "ajax": "1"})
    assert "inputText=HELLO" in seen["content"] and "ajax=1" in seen["content"]
```

- [ ] **Step 3: Run tests to verify they fail**

Run: `cd cli && python -m pytest tests/test_transport.py -v`
Expected: FAIL — `ModuleNotFoundError: splitflap_client.transport`

- [ ] **Step 4: Implement `transport.py`**

```python
"""HTTP transport for one board. No retries on mutations; verbatim error bodies."""
from __future__ import annotations

from typing import Any

import httpx


class SplitflapError(Exception):
    pass


class Unreachable(SplitflapError):
    def __init__(self, url: str, cause: Exception):
        super().__init__(f"unreachable: {url} ({cause})")
        self.url = url
        self.cause = cause


class HttpError(SplitflapError):
    def __init__(self, status: int, body: str, url: str):
        super().__init__(f"HTTP {status} from {url}: {body}")
        self.status = status
        self.body = body
        self.url = url


class ParseError(SplitflapError):
    pass


DEFAULT_TIMEOUT = httpx.Timeout(connect=2.0, read=5.0, write=5.0, pool=2.0)


class BoardClient:
    def __init__(self, base_url: str, *,
                 timeout: httpx.Timeout = DEFAULT_TIMEOUT,
                 transport: httpx.BaseTransport | None = None):
        self.base_url = base_url.rstrip("/")
        self._http = httpx.Client(base_url=self.base_url, timeout=timeout,
                                  transport=transport)

    def close(self) -> None:
        self._http.close()

    def __enter__(self) -> "BoardClient":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def _request(self, method: str, path: str, **kw) -> httpx.Response:
        try:
            resp = self._http.request(method, path, **kw)
        except httpx.TransportError as exc:
            raise Unreachable(f"{self.base_url}{path}", exc) from exc
        if resp.status_code >= 400:
            raise HttpError(resp.status_code, resp.text, str(resp.url))
        return resp

    def get_json(self, path: str, params: dict | None = None) -> Any:
        resp = self._request("GET", path, params=params)
        try:
            return resp.json()
        except ValueError as exc:
            raise ParseError(f"invalid JSON from {resp.url}") from exc

    def get_text(self, path: str, params: dict | None = None) -> str:
        return self._request("GET", path, params=params).text

    def post(self, path: str, *, params: dict | None = None,
             data: dict | None = None) -> httpx.Response:
        return self._request("POST", path, params=params, data=data)
```

Also write `cli/splitflap_client/__init__.py`:

```python
from .transport import (BoardClient, DEFAULT_TIMEOUT, HttpError, ParseError,
                        SplitflapError, Unreachable)
```

and empty `cli/splitflap_tui/__init__.py`, `cli/tests/__init__.py`.

- [ ] **Step 5: Run tests to verify they pass**

Run: `cd cli && pip install -e ".[dev]" && python -m pytest tests/test_transport.py -v`
Expected: 5 PASS

- [ ] **Step 6: Commit**

```bash
git add cli/
git commit -m "feat(#442): cli scaffold + BoardClient transport with verbatim-body errors"
```

---

### Task 2: Capability table + `/api` drift gate + capture script — #443

**Files:**
- Create: `cli/splitflap_client/capability.py`, `cli/tools/capture_fixtures.py`, `cli/tests/test_capability.py`, `cli/tests/fixtures/api_esp32s3.json`, `cli/tests/fixtures/api_esp01.json`

**Interfaces:**
- Consumes: nothing (pure).
- Produces: `PLAT_S3 = "esp32s3"`, `PLAT_ESP01 = "esp01"`; `plat_from_settings(settings: dict) -> str`; `CLIENT_ROUTES: dict[str, frozenset[tuple[str, str]]]` (per plat, `(method, path)` pairs the client is allowed to call); `ESP01_NOT_SERVED: frozenset[str]`; `serves(plat: str, method: str, path: str) -> bool`. Tasks 7/8/12 consume `serves()`.

The table lists only what the client **uses** — the drift gate proves each entry against the firmware's own `/api` index (`Master/ApiIndex.h:194-208` — `{"routes":[{"m","p","d"},…],"legend":{…}}`; follower adds `"notServed":[…]`, FollowerEsp01/ApiIndex.h:148-166).

- [ ] **Step 1: Capture the `/api` fixtures**

Write `cli/tools/capture_fixtures.py`:

```python
"""Capture live-board JSON into tests/fixtures/. Usage:
   python tools/capture_fixtures.py http://192.168.15.88 esp32s3
   python tools/capture_fixtures.py http://192.168.15.121 esp01
Review captures before committing (public repo)."""
import json
import pathlib
import sys

import httpx

FIXDIR = pathlib.Path(__file__).resolve().parent.parent / "tests" / "fixtures"
PATHS = {"api": "/api", "settings": "/settings", "units_health": "/units/health"}
S3_EXTRA = {"status": "/status", "cluster_status": "/cluster/status",
            "system_stats": "/system/stats"}
ESP01_EXTRA = {"cluster_health": "/cluster/health"}
REDACT = ("deviceName", "effectiveDeviceName", "mqttHost", "mqttUser")


def scrub(obj):
    if isinstance(obj, dict):
        return {k: ("" if k in REDACT else scrub(v)) for k, v in obj.items()}
    if isinstance(obj, list):
        return [scrub(v) for v in obj]
    return obj


def main() -> int:
    base, plat = sys.argv[1], sys.argv[2]
    paths = dict(PATHS, **(ESP01_EXTRA if plat == "esp01" else S3_EXTRA))
    FIXDIR.mkdir(parents=True, exist_ok=True)
    with httpx.Client(base_url=base, timeout=10.0) as http:
        for name, path in paths.items():
            data = scrub(http.get(path).json())
            out = FIXDIR / f"{name}_{plat}.json"
            out.write_text(json.dumps(data, indent=1, ensure_ascii=False) + "\n")
            print(f"wrote {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
```

Run it against both live boards (VPN must be up; leader `192.168.15.88`, esp01 row `192.168.15.121`):

```bash
cd cli && python tools/capture_fixtures.py http://192.168.15.88 esp32s3 \
        && python tools/capture_fixtures.py http://192.168.15.121 esp01
```

Fallback if a board is unreachable: transcribe `routes`/`notServed` by hand from `firmware/v2/Master/ApiIndex.h:22-76` / `firmware/v2/FollowerEsp01/ApiIndex.h` into the two `api_*.json` fixtures (`{"routes":[{"m":"GET","p":"/api","d":"…"},…]}`) and leave the other captures for later. Review every fixture for private strings before the commit step.

- [ ] **Step 2: Write the failing tests** (`cli/tests/test_capability.py`)

```python
import json
import pathlib

from splitflap_client.capability import (CLIENT_ROUTES, ESP01_NOT_SERVED,
                                         PLAT_ESP01, PLAT_S3,
                                         plat_from_settings, serves)

FIXDIR = pathlib.Path(__file__).parent / "fixtures"


def api_routes(plat):
    data = json.loads((FIXDIR / f"api_{plat}.json").read_text())
    return {(r["m"], r["p"]) for r in data["routes"]}, data


def test_plat_detection_defaults_to_s3():
    assert plat_from_settings({"plat": "esp01"}) == PLAT_ESP01
    assert plat_from_settings({"plat": "esp32s3"}) == PLAT_S3
    assert plat_from_settings({}) == PLAT_S3          # pre-#299 firmware
    assert plat_from_settings({"plat": ""}) == PLAT_S3


def test_every_client_route_is_served_s3():
    served, _ = api_routes(PLAT_S3)
    missing = CLIENT_ROUTES[PLAT_S3] - served
    assert not missing, f"client uses routes the S3 does not serve: {missing}"


def test_every_client_route_is_served_esp01():
    served, _ = api_routes(PLAT_ESP01)
    missing = CLIENT_ROUTES[PLAT_ESP01] - served
    assert not missing, f"client uses routes the esp01 does not serve: {missing}"


def test_not_served_matches_firmware_declaration():
    _, data = api_routes(PLAT_ESP01)
    assert set(data["notServed"]) == set(ESP01_NOT_SERVED)


def test_serves_gates_by_platform():
    assert serves(PLAT_S3, "POST", "/stop")
    assert not serves(PLAT_ESP01, "POST", "/stop")
    assert serves(PLAT_ESP01, "POST", "/unit/home")
    assert not serves(PLAT_ESP01, "GET", "/events")
```

- [ ] **Step 3: Run tests to verify they fail**

Run: `cd cli && python -m pytest tests/test_capability.py -v`
Expected: FAIL — `ModuleNotFoundError: splitflap_client.capability`

- [ ] **Step 4: Implement `capability.py`**

```python
"""What the client may call, per platform. NOT runtime-negotiated: two fixed
platforms exist; test_capability.py pins this table against the firmware's own
GET /api index (the same drift-gate idea as tests/test_api_index.py)."""
from __future__ import annotations

PLAT_S3 = "esp32s3"
PLAT_ESP01 = "esp01"


def plat_from_settings(settings: dict) -> str:
    # Absent/empty/unknown plat = S3-class board (ota-flash.sh precedent).
    return PLAT_ESP01 if settings.get("plat") == PLAT_ESP01 else PLAT_S3


_COMMON = frozenset({
    ("GET", "/api"), ("GET", "/settings"),
    ("GET", "/units/health"), ("POST", "/units/health/refresh"),
    ("GET", "/cluster/health"),
    ("GET", "/unit/offset"), ("POST", "/unit/offset"),
    ("POST", "/unit/jog"), ("POST", "/unit/home"), ("POST", "/unit/identify"),
    ("POST", "/unit/reset-odometer"), ("POST", "/unit/gates"),
    ("POST", "/unit/self-test"), ("GET", "/unit/self-test-result"),
    ("POST", "/unit/reboot"), ("GET", "/unit/op-result"),
    ("POST", "/reboot"),
})

CLIENT_ROUTES: dict[str, frozenset[tuple[str, str]]] = {
    PLAT_S3: _COMMON | frozenset({
        ("GET", "/status"), ("GET", "/system/stats"), ("GET", "/system/info"),
        ("GET", "/log/flash"), ("POST", "/log/flash/clear"),
        ("GET", "/coredump/summary"),
        ("GET", "/events"),
        ("GET", "/cluster/status"), ("GET", "/cluster/digest"),
        ("POST", "/cluster/discover"), ("GET", "/cluster/discover"),
        ("POST", "/cluster/config"), ("POST", "/cluster/promote"),
        ("POST", "/"), ("POST", "/stop"), ("POST", "/reset-units"),
        ("POST", "/unit/set-address"), ("POST", "/unit/clear-address"),
    }),
    PLAT_ESP01: _COMMON | frozenset({
        ("GET", "/log"),
    }),
}

# The follower's own declaration of deliberate absences (FollowerEsp01/ApiIndex.h).
ESP01_NOT_SERVED = frozenset({
    "/cluster/digest", "/cluster/promote", "/cluster/config", "/cluster/discover",
})


def serves(plat: str, method: str, path: str) -> bool:
    return (method, path) in CLIENT_ROUTES.get(plat, frozenset())
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `cd cli && python -m pytest tests/test_capability.py -v`
Expected: 5 PASS. If a drift assertion fails, the FIXTURE is truth — fix the table, not the fixture.

- [ ] **Step 6: Commit**

```bash
git add cli/splitflap_client/capability.py cli/tools/capture_fixtures.py cli/tests/test_capability.py cli/tests/fixtures/
git commit -m "feat(#443): per-plat capability table drift-gated against /api fixtures"
```

---

### Task 3: Settings model — #442

**Files:**
- Create: `cli/splitflap_client/models.py`, `cli/tests/test_models_settings.py`

**Interfaces:**
- Produces: `Settings` dataclass with `raw: dict` plus typed fields `plat, version, effective_device_name, device_mode, device_role, unit_count, cluster_state, cluster_leading, cluster_row, cluster_leader_name, heap, rssi, up, rescue_slot, rescue_slot_warn, last_written_text, last_reset_reason, reflash_on_boot`; classmethod `Settings.from_json(d: dict) -> Settings`. Also module helpers `_int(d, key, default=0)`, `_str(d, key, default="")`, `_bool(d, key, default=False)` reused by Tasks 4-5.
- Tolerance rule (applies to every model in this plan): missing/wrong-typed keys fall back to the default — never raise; `raw` keeps the whole dict so panels can show keys the model doesn't type.

The esp01 `/settings` uses `width` where the S3 uses `unitCount`, and its `clusterState` vocabulary is `standalone|clustered|grace|blank` vs the S3's `…|local-fallback` (FollowerJson.h:227-253) — `from_json` maps both.

- [ ] **Step 1: Write the failing tests** (`cli/tests/test_models_settings.py`)

```python
from splitflap_client.models import Settings

S3 = {"unitCount": 16, "deviceMode": "clock", "deviceRole": "display",
      "version": "817e3a9", "effectiveDeviceName": "splitflap-a1b2",
      "clusterState": "standalone", "clusterLeading": True, "clusterRow": 1,
      "clusterLeaderName": "", "rescueSlot": "ok", "rescueSlotWarn": False,
      "lastWrittenText": "HELLO", "lastResetReason": "POWERON_RESET",
      "reflashOnBoot": False, "heap": 180000, "rssi": -52, "up": 3600,
      "plat": "esp32s3"}

ESP01 = {"deviceName": "", "effectiveDeviceName": "splitflap-01ab",
         "version": "9f694dd", "width": 5, "clusterState": "blank",
         "clusterLeaderName": "row1", "clusterLeaderHost": "192.168.15.88",
         "clusterRow": 0, "plat": "esp01", "heap": 21000, "rssi": -63, "up": 900}


def test_s3_settings_parse():
    s = Settings.from_json(S3)
    assert s.plat == "esp32s3" and s.unit_count == 16
    assert s.cluster_leading is True and s.device_mode == "clock"
    assert s.rescue_slot == "ok" and s.raw["lastWrittenText"] == "HELLO"


def test_esp01_width_maps_to_unit_count():
    s = Settings.from_json(ESP01)
    assert s.plat == "esp01" and s.unit_count == 5
    assert s.cluster_state == "blank" and s.cluster_leading is False


def test_missing_keys_never_raise():
    s = Settings.from_json({})
    assert s.plat == "esp32s3" and s.unit_count == 0 and s.version == ""
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd cli && python -m pytest tests/test_models_settings.py -v`
Expected: FAIL — no `models` module

- [ ] **Step 3: Implement in `models.py`**

```python
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
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd cli && python -m pytest tests/test_models_settings.py -v`
Expected: 3 PASS

- [ ] **Step 5: Commit**

```bash
git add cli/splitflap_client/models.py cli/tests/test_models_settings.py
git commit -m "feat(#442): Settings model with esp01 width/vocabulary mapping"
```

---

### Task 4: Units-health model — #442

**Files:**
- Modify: `cli/splitflap_client/models.py` (append)
- Create: `cli/tests/test_models_units.py`

**Interfaces:**
- Consumes: `_int/_str/_bool` from Task 3.
- Produces: `UnitEntry` (fields `index, address, state, valid, raw`; `Optional[int]` properties `sx, sxl, odo, vcc_min, gates, hall_fails, err, err_age, age, misses`; `rev: str`; `stale: bool`; `fault: bool` = the firmware's per-unit fault predicate proxy: `state != 1 or stale`); `UnitsHealth` (fields `width, faulty, vcc_min: int | None, units: list[UnitEntry], wear_flagged: list[int], reflash_state: str, reflash_halted: bool, raw`); `UnitsHealth.from_json(d) -> UnitsHealth`.

Per-unit key emission is conditional on independent validity flags (shared/UnitHealth.h:295-412): `i,a,st,v` always; `sx…` only with extDiag; `hf,gates,sxl` only when lifetime-valid AND non-zero; `err/errAge` master-only; `stale` is the literal `1` when latched. `Optional` properties return `None` when the key is absent — the TUI renders `—`, never 0, for absent.

- [ ] **Step 1: Write the failing tests** (`cli/tests/test_models_units.py`)

```python
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
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd cli && python -m pytest tests/test_models_units.py -v`
Expected: FAIL — `UnitsHealth` not defined

- [ ] **Step 3: Append to `models.py`**

```python
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
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd cli && python -m pytest tests/test_models_units.py -v`
Expected: 3 PASS

- [ ] **Step 5: Commit**

```bash
git add cli/splitflap_client/models.py cli/tests/test_models_units.py
git commit -m "feat(#442): UnitsHealth model with validity-gated optional keys"
```

---

### Task 5: Cluster + stats + ota + `/status` aggregate models — #442

**Files:**
- Modify: `cli/splitflap_client/models.py` (append)
- Create: `cli/tests/test_models_cluster.py`

**Interfaces:**
- Consumes: `Settings`, `UnitsHealth`, `_int/_str/_bool/_opt`.
- Produces:
  - `ClusterMember` (`host, self_row: bool, row, col, width, joined, degraded, failures, rev, plat` — absent `plat` key resolves to `"esp32s3"` (ClusterDigest.h:110-114), `role, suspect, rescue, render_stuck, updating, update_blocked, hmac, faulty: int | None, raw`)
  - `ClusterStatus` (`enabled, epoch, seq, members: list[ClusterMember], rollout_phase, rollout_src, follower_image_present, follower_image_rev, raw`), `.from_json(d)`
  - `ClusterHealth` (`state, leader_name, leader_host, row, rev, hmac, foreign_joins, foreign_pings, foreign_renders, foreign_last_host, stack_free: int | None, raw`), `.from_json(d)` — works for both platforms' `/cluster/health` (`stackFree` is esp01-only, FollowerJson.h:172-221)
  - `SystemStatsNow` (`rssi, heap, min_heap, cpu0, cpu1, temp_dc, uptime, i2c_tx, i2c_err, ntp_age, reset, hwm: dict[str, int], raw`), `.from_json(d)` — parses the `now` object (SystemStatsPolicy.h:108)
  - `OtaDebug` (`running, next, last_invalid: str | None, last_flash_result, ota_reverted, factory_valid, raw`), `.from_json(d)`
  - `StatusAggregate` (`settings: Settings, stats_now: SystemStatsNow, units: UnitsHealth, cluster: ClusterStatus, ota: OtaDebug, raw`), `.from_json(d)` — the one-shot `/status` (WebSystem.cpp:318-349)

- [ ] **Step 1: Write the failing tests** (`cli/tests/test_models_cluster.py`)

```python
from splitflap_client.models import (ClusterHealth, ClusterStatus,
                                     StatusAggregate)

CS = {"enabled": True, "epoch": 7, "seq": 1234,
      "members": [
          {"host": "", "self": True, "row": 1, "col": 0, "width": 16,
           "joined": True, "degraded": False, "failures": 0, "rev": "817e3a9",
           "reportedWidth": 16, "updating": False, "updateBlocked": False,
           "hmac": True},
          {"host": "192.168.15.121", "self": False, "row": 0, "col": 0,
           "width": 5, "joined": True, "degraded": False, "failures": 1,
           "rev": "9f694dd", "plat": "esp01", "role": "display",
           "faulty": 0, "detected": 5, "faultMask": "0000", "wear": False,
           "updating": False, "updateBlocked": False, "suspect": True,
           "rescue": True, "hmac": True},
      ],
      "rollout": {"phase": "idle", "host": "", "sent": 0, "total": 0,
                  "imageVerifyFailed": False},
      "followerImage": {"present": True, "rev": "9f694dd"},
      "followerPush": {"phase": "idle", "host": "", "sent": 0, "total": 0,
                       "result": "none"}}


def test_cluster_status_parse():
    c = ClusterStatus.from_json(CS)
    assert c.enabled and c.epoch == 7 and len(c.members) == 2
    own, esp = c.members
    assert own.self_row and own.plat == "esp32s3" and not own.suspect
    assert esp.plat == "esp01" and esp.suspect and esp.rescue and esp.faulty == 0
    assert c.follower_image_present and c.follower_image_rev == "9f694dd"


def test_member_optional_health_block_absent():
    c = ClusterStatus.from_json({"enabled": True, "epoch": 1, "seq": 1,
                                 "members": [{"host": "x", "row": 0, "col": 0,
                                              "width": 5, "joined": False,
                                              "degraded": True, "failures": 9,
                                              "rev": "", "hmac": False}]})
    m = c.members[0]
    assert m.faulty is None and not m.suspect and not m.rescue


def test_cluster_health_both_platforms():
    esp01 = {"state": "clustered", "leaderName": "row1",
             "leaderHost": "192.168.15.88", "row": 0, "rev": "9f694dd",
             "hmac": True, "stackFree": 1200,
             "foreign": {"joins": 2, "pings": 0, "renders": 0,
                         "lastHost": "192.168.15.20", "msSince": 51000}}
    h = ClusterHealth.from_json(esp01)
    assert h.stack_free == 1200 and h.foreign_joins == 2
    s3 = {"state": "standalone", "leaderName": "", "leaderHost": "", "row": 0,
          "rev": "817e3a9", "hmac": False,
          "foreign": {"joins": 0, "pings": 0, "renders": 0, "lastHost": "",
                      "msSince": -1}}
    assert ClusterHealth.from_json(s3).stack_free is None


def test_status_aggregate_splices():
    agg = {"settings": {"plat": "esp32s3", "unitCount": 16, "version": "817e3a9",
                        "clusterLeading": True},
           "stats": {"now": {"rssi": -52, "heap": 180000, "minHeap": 150000,
                             "uptime": 3600, "i2cTx": 9001, "i2cErr": 3,
                             "ntpAge": 42, "reset": "POWERON_RESET",
                             "hwm": {"display": 2100, "cluster": 2600}}},
           "units": {"width": 16, "faulty": 0, "units": []},
           "cluster": CS,
           "ota": {"running": "app0", "next": "app1", "lastInvalid": None,
                   "lastFlashResult": "", "otaReverted": False,
                   "factoryValid": True}}
    s = StatusAggregate.from_json(agg)
    assert s.settings.cluster_leading and s.stats_now.hwm["cluster"] == 2600
    assert s.ota.running == "app0" and s.cluster.epoch == 7
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd cli && python -m pytest tests/test_models_cluster.py -v`
Expected: FAIL — names not defined

- [ ] **Step 3: Append to `models.py`**

```python
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
                        if isinstance(v, (int, float))})


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
        stats = d.get("stats") if isinstance(d.get("stats"), dict) else {}
        return cls(
            raw=d,
            settings=Settings.from_json(d.get("settings") or {}),
            stats_now=SystemStatsNow.from_json(stats.get("now") or {}),
            units=UnitsHealth.from_json(d.get("units") or {}),
            cluster=ClusterStatus.from_json(d.get("cluster") or {}),
            ota=OtaDebug.from_json(d.get("ota") or {}),
        )
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd cli && python -m pytest tests/test_models_cluster.py tests/ -v`
Expected: all PASS (including earlier suites)

- [ ] **Step 5: Add fixture parse-smoke test** — append to `test_models_cluster.py`:

```python
import json
import pathlib

import pytest

FIXDIR = pathlib.Path(__file__).parent / "fixtures"


@pytest.mark.parametrize("name,cls", [
    ("settings_esp32s3", "Settings"), ("settings_esp01", "Settings"),
    ("units_health_esp32s3", "UnitsHealth"), ("units_health_esp01", "UnitsHealth"),
    ("status_esp32s3", "StatusAggregate"),
    ("cluster_status_esp32s3", "ClusterStatus"),
    ("cluster_health_esp01", "ClusterHealth"),
])
def test_live_fixture_parses(name, cls):
    import splitflap_client.models as models
    path = FIXDIR / f"{name}.json"
    if not path.exists():
        pytest.skip(f"fixture {name} not captured yet")
    getattr(models, cls).from_json(json.loads(path.read_text()))
```

Run: `cd cli && python -m pytest tests/test_models_cluster.py -v` — captured fixtures parse, missing ones skip.

- [ ] **Step 6: Commit**

```bash
git add cli/splitflap_client/models.py cli/tests/test_models_cluster.py
git commit -m "feat(#442): cluster/stats/ota models + /status aggregate + fixture smoke tests"
```

---

### Task 6: Logs — S3 flash log + esp01 ring cursor — #442

**Files:**
- Create: `cli/splitflap_client/logs.py`, `cli/tests/test_logs.py`

**Interfaces:**
- Consumes: `BoardClient`, `HttpError`.
- Produces: `fetch_flash_log(client, prev=False) -> str` (S3: `GET /log/flash`, `?prev` when prev; 404 "No flash log yet" → returns `""` — benign rotation race, WebSystem.cpp:64-77; other HttpError re-raised); `FollowerLogDelta(cursor: int, text: str)`; `fetch_follower_log(client, after: int = 0) -> FollowerLogDelta` (esp01 `GET /log?after=N`; body = `"<nextCursor>\n<delta>"`, FollowerLog.h:24-64; a rebooted follower rewinds the cursor — callers just store `delta.cursor`).

- [ ] **Step 1: Write the failing tests** (`cli/tests/test_logs.py`)

```python
import httpx
import pytest
from splitflap_client.logs import (FollowerLogDelta, fetch_flash_log,
                                   fetch_follower_log)
from splitflap_client.transport import BoardClient, HttpError


def make_client(handler):
    return BoardClient("http://b", transport=httpx.MockTransport(handler))


def test_flash_log_plain_and_prev_param():
    seen = {}
    def handler(req):
        seen["params"] = dict(req.url.params)
        return httpx.Response(200, text="line1\nline2\n")
    c = make_client(handler)
    assert fetch_flash_log(c) == "line1\nline2\n"
    assert seen["params"] == {}
    fetch_flash_log(c, prev=True)
    assert "prev" in seen["params"]


def test_flash_log_404_is_empty_not_error():
    c = make_client(lambda r: httpx.Response(404, text="No flash log yet"))
    assert fetch_flash_log(c) == ""


def test_flash_log_503_raises():
    c = make_client(lambda r: httpx.Response(503, text="Flash log unavailable"))
    with pytest.raises(HttpError):
        fetch_flash_log(c)


def test_follower_log_cursor_parse():
    def handler(req):
        assert req.url.params["after"] == "100"
        return httpx.Response(200, text="164\nboot ok\njoined leader\n")
    d = fetch_follower_log(make_client(handler), after=100)
    assert d == FollowerLogDelta(cursor=164, text="boot ok\njoined leader\n")


def test_follower_log_empty_delta():
    d = fetch_follower_log(make_client(lambda r: httpx.Response(200, text="164\n")))
    assert d.cursor == 164 and d.text == ""
```

- [ ] **Step 2: Run to verify FAIL** — `cd cli && python -m pytest tests/test_logs.py -v` → no `logs` module.

- [ ] **Step 3: Implement `logs.py`**

```python
"""Log retrieval. S3: LittleFS flash log (plain text). esp01: 2 KB RAM ring
with a monotonic byte cursor (FollowerLog.h) — first line of the body is the
next cursor, the rest is the delta."""
from __future__ import annotations

from dataclasses import dataclass

from .transport import BoardClient, HttpError, ParseError


def fetch_flash_log(client: BoardClient, prev: bool = False) -> str:
    params = {"prev": "1"} if prev else None
    try:
        return client.get_text("/log/flash", params=params)
    except HttpError as exc:
        if exc.status == 404:      # rotation race / no log yet — benign
            return ""
        raise


@dataclass(frozen=True)
class FollowerLogDelta:
    cursor: int
    text: str


def fetch_follower_log(client: BoardClient, after: int = 0) -> FollowerLogDelta:
    body = client.get_text("/log", params={"after": str(after)})
    head, sep, rest = body.partition("\n")
    try:
        cursor = int(head.strip())
    except ValueError as exc:
        raise ParseError(f"bad follower log cursor line: {head!r}") from exc
    return FollowerLogDelta(cursor=cursor, text=rest if sep else "")
```

- [ ] **Step 4: Run to verify PASS** — `cd cli && python -m pytest tests/test_logs.py -v` → 5 PASS

- [ ] **Step 5: Commit**

```bash
git add cli/splitflap_client/logs.py cli/tests/test_logs.py
git commit -m "feat(#442): flash-log fetch + esp01 ring-cursor parsing"
```

---

### Task 7: Op contract — submit, op-result, self-test-result — #444

**Files:**
- Create: `cli/splitflap_client/ops.py`, `cli/tests/test_ops.py`

**Interfaces:**
- Consumes: `BoardClient`, `HttpError`, `ParseError`.
- Produces:
  - `OpResult(state: str, reason: str | None = None, detail: str | None = None)`; `parse_op_result(d) -> OpResult`. States exactly: `pending|expired|ok|failed` (DisplayIpc.h:352-374; follower byte-identical, FollowerOps.h:171-193).
  - `SelfTestResult(state, reason, unit_reason, steps_per_rev, hall_window, rev_time_ms)`; `parse_self_test_result(d)` (DisplayIpc.h:318-348).
  - `submit_op(client, path: str, params: dict) -> int` — POSTs, parses `{"seq":N}`, returns N. 409/503 propagate as `HttpError` (verbatim body).
  - `wait_op(client, seq, *, result_path="/unit/op-result", timeout_s=30.0, poll_s=0.5, sleep=time.sleep, clock=time.monotonic) -> OpResult` — polls until non-pending or timeout (timeout returns the last pending as `OpResult("pending")`; caller decides). `sleep`/`clock` injectable for tests.
  - `run_op(client, path, params, **wait_kw) -> OpResult` = submit + wait.
  - Op param names (WebMaintenance.cpp:181-269): offset=`address`+`value`, jog=`address`+`steps`, gates=`address`+`gates`, home/identify/reset-odometer/self-test/reboot/set-address/clear-address=`address` (+set-address `target`— verify exact name at WebMaintenance.cpp:305 during implementation and mirror it in `commands.py`), `/reset-units` and `/stop` take none.

- [ ] **Step 1: Write the failing tests** (`cli/tests/test_ops.py`)

```python
import httpx
import pytest
from splitflap_client.ops import (OpResult, SelfTestResult, parse_op_result,
                                  parse_self_test_result, run_op, submit_op,
                                  wait_op)
from splitflap_client.transport import BoardClient, HttpError


def make_client(handler):
    return BoardClient("http://b", transport=httpx.MockTransport(handler))


def test_parse_op_result_all_shapes():
    assert parse_op_result({"state": "pending"}) == OpResult("pending")
    assert parse_op_result({"state": "ok"}) == OpResult("ok")
    assert parse_op_result({"state": "expired"}) == OpResult("expired")
    r = parse_op_result({"state": "failed", "reason": "postcondition-fail",
                         "detail": "unit-missing-after-reprobe"})
    assert r.state == "failed" and r.detail == "unit-missing-after-reprobe"


def test_submit_returns_seq_and_sends_query_params():
    def handler(req):
        assert req.url.params["address"] == "3"
        return httpx.Response(200, json={"seq": 41})
    assert submit_op(make_client(handler), "/unit/home", {"address": 3}) == 41


def test_submit_409_propagates_verbatim():
    body = "Unit reflash in progress — retry when it finishes"
    c = make_client(lambda r: httpx.Response(409, text=body))
    with pytest.raises(HttpError) as exc:
        submit_op(c, "/unit/home", {"address": 3})
    assert exc.value.body == body


def test_wait_op_polls_until_ok():
    answers = [{"state": "pending"}, {"state": "pending"}, {"state": "ok"}]
    def handler(req):
        assert req.url.params["seq"] == "41"
        return httpx.Response(200, json=answers.pop(0))
    naps = []
    r = wait_op(make_client(handler), 41, sleep=naps.append,
                clock=lambda: len(naps) * 0.5)
    assert r == OpResult("ok") and len(naps) == 2


def test_wait_op_timeout_returns_pending():
    c = make_client(lambda r: httpx.Response(200, json={"state": "pending"}))
    ticks = iter([0, 1, 2, 31, 32])
    r = wait_op(c, 7, timeout_s=30.0, sleep=lambda s: None,
                clock=lambda: next(ticks))
    assert r == OpResult("pending")


def test_self_test_result_parse():
    ok = parse_self_test_result({"state": "ok", "steps_per_rev": 2038,
                                 "hall_window": 46, "rev_time_ms": 1200})
    assert ok.steps_per_rev == 2038 and ok.state == "ok"
    bad = parse_self_test_result({"state": "failed", "reason": "unit-failed",
                                  "unit_reason": "hall-stuck",
                                  "steps_per_rev": 0, "hall_window": 0,
                                  "rev_time_ms": 0})
    assert bad.unit_reason == "hall-stuck"


def test_run_op_end_to_end():
    calls = {"n": 0}
    def handler(req):
        if req.url.path == "/unit/identify":
            return httpx.Response(200, json={"seq": 5})
        calls["n"] += 1
        state = {"state": "pending"} if calls["n"] < 2 else {"state": "ok"}
        return httpx.Response(200, json=state)
    r = run_op(make_client(handler), "/unit/identify", {"address": 2},
               sleep=lambda s: None, clock=lambda: calls["n"] * 0.1)
    assert r.state == "ok"
```

- [ ] **Step 2: Run to verify FAIL** — `cd cli && python -m pytest tests/test_ops.py -v` → no `ops` module.

- [ ] **Step 3: Implement `ops.py`**

```python
"""The {"seq":N} op contract: submit, then poll /unit/op-result. Single result
slot on the firmware side, last-op-wins — expired means the slot moved on."""
from __future__ import annotations

import time
from dataclasses import dataclass
from typing import Callable

from .transport import BoardClient, ParseError


@dataclass(frozen=True)
class OpResult:
    state: str                       # pending | expired | ok | failed
    reason: str | None = None
    detail: str | None = None


@dataclass(frozen=True)
class SelfTestResult:
    state: str
    reason: str | None
    unit_reason: str | None
    steps_per_rev: int
    hall_window: int
    rev_time_ms: int


def parse_op_result(d: dict) -> OpResult:
    return OpResult(state=str(d.get("state", "")),
                    reason=d.get("reason"), detail=d.get("detail"))


def parse_self_test_result(d: dict) -> SelfTestResult:
    return SelfTestResult(state=str(d.get("state", "")),
                          reason=d.get("reason"),
                          unit_reason=d.get("unit_reason"),
                          steps_per_rev=int(d.get("steps_per_rev", 0) or 0),
                          hall_window=int(d.get("hall_window", 0) or 0),
                          rev_time_ms=int(d.get("rev_time_ms", 0) or 0))


def submit_op(client: BoardClient, path: str, params: dict) -> int:
    resp = client.post(path, params={k: str(v) for k, v in params.items()})
    try:
        return int(resp.json()["seq"])
    except (ValueError, KeyError, TypeError) as exc:
        raise ParseError(f"no seq in op reply from {path}: {resp.text!r}") from exc


def wait_op(client: BoardClient, seq: int, *,
            result_path: str = "/unit/op-result",
            timeout_s: float = 30.0, poll_s: float = 0.5,
            sleep: Callable[[float], None] = time.sleep,
            clock: Callable[[], float] = time.monotonic) -> OpResult:
    deadline = clock() + timeout_s
    while True:
        raw = client.get_json(result_path, params={"seq": str(seq)})
        result = parse_op_result(raw if isinstance(raw, dict) else {})
        if result.state != "pending" or clock() >= deadline:
            return result
        sleep(poll_s)


def run_op(client: BoardClient, path: str, params: dict, **wait_kw) -> OpResult:
    return wait_op(client, submit_op(client, path, params), **wait_kw)
```

- [ ] **Step 4: Run to verify PASS** — `cd cli && python -m pytest tests/test_ops.py -v` → 7 PASS

- [ ] **Step 5: Verify the set-address param name** — read `firmware/v2/Master/WebMaintenance.cpp:305-338`, note the exact query-param names for `/unit/set-address` (source + target address) in a comment atop `ops.py`. This feeds `commands.py` in Task 12.

- [ ] **Step 6: Commit**

```bash
git add cli/splitflap_client/ops.py cli/tests/test_ops.py
git commit -m "feat(#444): seq op contract with injectable-clock polling + self-test results"
```

---

### Task 8: Display control + settings writes (`control.py`) — #445 groundwork

**Files:**
- Create: `cli/splitflap_client/control.py`, `cli/tests/test_control.py`

**Interfaces:**
- Consumes: `BoardClient`, `HttpError`, `submit_op`.
- Produces: `post_form(client, **fields) -> str` (adds `ajax=1`, POSTs `/`, returns body — `"ok"` or `"ok-reboot"`; 400 body `"invalid"`, 409 `"reflash in progress"|"clustered"`, 503 `"display busy"` arrive as `HttpError` with those verbatim bodies, WebSettings.cpp:35-125); `set_text(client, text)`; `set_mode(client, mode)`; `notify(client, text, dwell_s)` (transientText+transientDwell); `set_setting(client, field, value)` (raw passthrough — field names are the firmware's own: `alignment`, `flapSpeed`, `timezone`, `deviceName`, `deviceRole`, `unitCount`, `reflashOnBoot`, `mqttHost`, `mqttPort`, `mqttUser`, `mqttPassword`, PendingSettingsPost.h:29-43); `stop(client) -> int` (POST `/stop`, returns seq — Master-only, blanks the wall when leading); `reboot(client) -> None` (POST `/reboot`).

- [ ] **Step 1: Write the failing tests** (`cli/tests/test_control.py`)

```python
import httpx
import pytest
from splitflap_client.control import (notify, post_form, reboot, set_mode,
                                      set_text, stop)
from splitflap_client.transport import BoardClient, HttpError


def capture():
    seen = {}
    def handler(req):
        seen["path"] = req.url.path
        seen["form"] = dict(httpx.QueryParams(req.content.decode()))
        return httpx.Response(200, text="ok")
    return seen, BoardClient("http://b", transport=httpx.MockTransport(handler))


def test_ajax_always_set_and_text_field():
    seen, c = capture()
    assert set_text(c, "TRAINS LATE") == "ok"
    assert seen["path"] == "/" and seen["form"]["ajax"] == "1"
    assert seen["form"]["inputText"] == "TRAINS LATE"


def test_notify_sends_transient_pair():
    seen, c = capture()
    notify(c, "DOOR", 15)
    assert seen["form"]["transientText"] == "DOOR"
    assert seen["form"]["transientDwell"] == "15"


def test_mode_field():
    seen, c = capture()
    set_mode(c, "clock")
    assert seen["form"]["deviceMode"] == "clock"


def test_clustered_409_verbatim():
    c = BoardClient("http://b", transport=httpx.MockTransport(
        lambda r: httpx.Response(409, text="clustered")))
    with pytest.raises(HttpError) as exc:
        set_text(c, "X")
    assert exc.value.body == "clustered"


def test_stop_returns_seq():
    c = BoardClient("http://b", transport=httpx.MockTransport(
        lambda r: httpx.Response(200, json={"seq": 9})))
    assert stop(c) == 9


def test_reboot_posts():
    seen = {}
    def handler(req):
        seen["path"] = req.url.path
        return httpx.Response(200, text="rebooting")
    reboot(BoardClient("http://b", transport=httpx.MockTransport(handler)))
    assert seen["path"] == "/reboot"
```

- [ ] **Step 2: Run to verify FAIL** — `cd cli && python -m pytest tests/test_control.py -v`

- [ ] **Step 3: Implement `control.py`**

```python
"""Display + settings writes. Everything rides the v1-compatible form POST /
with ajax=1 (WebSettings.cpp) — there is no JSON settings route by design."""
from __future__ import annotations

from .ops import submit_op
from .transport import BoardClient


def post_form(client: BoardClient, **fields: str) -> str:
    data = {k: str(v) for k, v in fields.items()}
    data["ajax"] = "1"
    return client.post("/", data=data).text


def set_text(client: BoardClient, text: str) -> str:
    return post_form(client, inputText=text)


def set_mode(client: BoardClient, mode: str) -> str:
    return post_form(client, deviceMode=mode)


def notify(client: BoardClient, text: str, dwell_s: int) -> str:
    return post_form(client, transientText=text, transientDwell=dwell_s)


def set_setting(client: BoardClient, field: str, value: str) -> str:
    return post_form(client, **{field: value})


def stop(client: BoardClient) -> int:
    return submit_op(client, "/stop", {})


def reboot(client: BoardClient) -> None:
    client.post("/reboot")
```

- [ ] **Step 4: Run to verify PASS** — `cd cli && python -m pytest tests/test_control.py -v` → 6 PASS

- [ ] **Step 5: Commit**

```bash
git add cli/splitflap_client/control.py cli/tests/test_control.py
git commit -m "feat(#445): display/settings control over the ajax form POST contract"
```

---

### Task 9: SSE consumer (`events.py`) — #444

**Files:**
- Create: `cli/splitflap_client/events.py`, `cli/tests/test_events.py`

**Interfaces:**
- Consumes: `BoardClient` (its private `_http` for streaming — add a public `stream(path)` helper to `transport.py` instead of reaching in).
- Produces: `DisplayEvent(text: str, self_row: int | None, rows: list[str] | None)`; `display_events(client) -> Iterator[DisplayEvent]` — one connection, yields per `display` event; raises `Unreachable` when the stream drops (caller loop reconnects with backoff — reconnection policy lives in the TUI poller, keeping this generator testable). Wire format: `event: display` / `data: {json}` / blank-line separator; first event arrives immediately on connect (WebContent.cpp:64-69, DisplayEvents.h:61-78).

- [ ] **Step 1: Add the transport streaming seam** — append to `BoardClient` in `transport.py`:

```python
    def stream(self, path: str):
        """Context manager for a long-lived GET (SSE): read timeout disabled."""
        timeout = httpx.Timeout(connect=2.0, read=None, write=5.0, pool=2.0)
        return self._http.stream("GET", path, timeout=timeout)
```

- [ ] **Step 2: Write the failing tests** (`cli/tests/test_events.py`)

```python
import httpx
import pytest
from splitflap_client.events import DisplayEvent, display_events
from splitflap_client.transport import BoardClient, Unreachable

STREAM = (b"event: display\n"
          b'data: {"text":"HELLO"}\n'
          b"\n"
          b"event: display\n"
          b'data: {"text":"WALL","selfRow":1,"rows":["ROW0","WALL"]}\n'
          b"\n")


def make_client(content=STREAM):
    def handler(req):
        return httpx.Response(200, content=content,
                              headers={"content-type": "text/event-stream"})
    return BoardClient("http://b", transport=httpx.MockTransport(handler))


def test_yields_display_events_and_wall_rows():
    events = list(display_events(make_client()))
    assert events[0] == DisplayEvent(text="HELLO", self_row=None, rows=None)
    assert events[1] == DisplayEvent(text="WALL", self_row=1,
                                     rows=["ROW0", "WALL"])


def test_ignores_unknown_fields_and_bad_json():
    content = (b"id: 123\nevent: display\ndata: not-json\n\n"
               b'event: display\ndata: {"text":"OK"}\n\n')
    events = list(display_events(make_client(content)))
    assert events == [DisplayEvent(text="OK", self_row=None, rows=None)]


def test_connect_failure_raises_unreachable():
    def handler(req):
        raise httpx.ConnectError("down")
    c = BoardClient("http://b", transport=httpx.MockTransport(handler))
    with pytest.raises(Unreachable):
        list(display_events(c))
```

- [ ] **Step 3: Run to verify FAIL** — `cd cli && python -m pytest tests/test_events.py -v`

- [ ] **Step 4: Implement `events.py`**

```python
"""SSE /events consumer (S3 only; single event name 'display'). One
connection per call — the TUI poller owns reconnect/backoff."""
from __future__ import annotations

import json
from dataclasses import dataclass
from typing import Iterator

import httpx

from .transport import BoardClient, Unreachable


@dataclass(frozen=True)
class DisplayEvent:
    text: str
    self_row: int | None      # only while the board leads a wall
    rows: list[str] | None


def _parse(data: str) -> DisplayEvent | None:
    try:
        d = json.loads(data)
    except ValueError:
        return None
    if not isinstance(d, dict) or not isinstance(d.get("text"), str):
        return None
    rows = d.get("rows")
    return DisplayEvent(
        text=d["text"],
        self_row=d["selfRow"] if isinstance(d.get("selfRow"), int) else None,
        rows=[str(r) for r in rows] if isinstance(rows, list) else None)


def display_events(client: BoardClient) -> Iterator[DisplayEvent]:
    try:
        with client.stream("/events") as resp:
            event_name, data_lines = "", []
            for line in resp.iter_lines():
                if line == "":
                    if event_name == "display" and data_lines:
                        parsed = _parse("\n".join(data_lines))
                        if parsed is not None:
                            yield parsed
                    event_name, data_lines = "", []
                elif line.startswith("event:"):
                    event_name = line[6:].strip()
                elif line.startswith("data:"):
                    data_lines.append(line[5:].strip())
    except httpx.TransportError as exc:
        raise Unreachable(f"{client.base_url}/events", exc) from exc
```

- [ ] **Step 5: Run to verify PASS** — `cd cli && python -m pytest tests/test_events.py tests/test_transport.py -v` → all PASS

- [ ] **Step 6: Commit**

```bash
git add cli/splitflap_client/transport.py cli/splitflap_client/events.py cli/tests/test_events.py
git commit -m "feat(#444): SSE display-events consumer + transport stream seam"
```

---

### Task 10: Wire-twin integration test — #443

**Files:**
- Create: `cli/tests/test_wire_twin.py`

**Interfaces:**
- Consumes: everything from Tasks 1-9; `firmware/v2/Master/tests/fake_follower.py` (the pytest-pinned wire twin; `--plat esp01` variant).

The twin serves the follower-side cluster wire. It pins the client's parsing of a real (not hand-written) server for the endpoints both serve. Read `fake_follower.py`'s argparse header first to get its invocation (port flag, plat flag) and which of `/settings`, `/units/health`, `/cluster/health` it serves — assert against exactly those; skip cleanly if the twin doesn't serve one.

- [ ] **Step 1: Write the test** (auto-skips when the twin can't start)

```python
"""Client vs the repo's fake_follower wire twin — the same twin that pins the
firmware, so client parsing can't drift from the wire."""
import json
import pathlib
import socket
import subprocess
import sys
import time
import urllib.request

import pytest

from splitflap_client.models import ClusterHealth, Settings
from splitflap_client.transport import BoardClient

TWIN = (pathlib.Path(__file__).resolve().parents[2]
        / "firmware/v2/Master/tests/fake_follower.py")


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


@pytest.fixture(scope="module")
def twin_url():
    if not TWIN.exists():
        pytest.skip("fake_follower.py not present")
    port = free_port()
    proc = subprocess.Popen([sys.executable, str(TWIN), "--port", str(port),
                             "--plat", "esp01"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    url = f"http://127.0.0.1:{port}"
    for _ in range(50):
        try:
            urllib.request.urlopen(url + "/settings", timeout=0.2)
            break
        except OSError:
            if proc.poll() is not None:
                pytest.skip("twin exited at startup")
            time.sleep(0.1)
    else:
        proc.kill()
        pytest.skip("twin never came up")
    yield url
    proc.terminate()
    proc.wait(timeout=5)


def test_settings_parse_from_twin(twin_url):
    with BoardClient(twin_url) as c:
        s = Settings.from_json(c.get_json("/settings"))
    assert s.plat == "esp01"


def test_cluster_health_parse_from_twin(twin_url):
    with BoardClient(twin_url) as c:
        h = ClusterHealth.from_json(c.get_json("/cluster/health"))
    assert h.state != ""
```

- [ ] **Step 2: Check the twin's real CLI** — read the argparse block at the top of `firmware/v2/Master/tests/fake_follower.py`; fix the flags in the fixture (`--port`/`--plat` names) and the endpoint list to match what it actually serves. If it doesn't serve `/cluster/health` or `/settings`, swap the asserts to endpoints it does serve (its pytest twin `firmware/v2/FollowerEsp01/tests/fake_leader.py` shows the expected surface).

- [ ] **Step 3: Run** — `cd cli && python -m pytest tests/test_wire_twin.py -v`
Expected: 2 PASS (or SKIP with a clear reason on machines without the twin)

- [ ] **Step 4: Commit**

```bash
git add cli/tests/test_wire_twin.py
git commit -m "test(#443): client parsing pinned against the fake_follower wire twin"
```

---

### Task 11: Config + TUI dashboard skeleton — #445, #447

**Files:**
- Create: `cli/splitflap_tui/config.py`, `cli/splitflap_tui/app.py`, `cli/splitflap_tui/poller.py`, `cli/splitflap_tui/widgets.py`, `cli/splitflap_tui/__main__.py`, `cli/tests/test_config.py`, `cli/tests/test_app.py`

**Interfaces:**
- Consumes: `BoardClient`, `StatusAggregate`, `ClusterStatus`, `display_events`, `fetch_flash_log`.
- Produces:
  - `config.py`: `Board(name: str, url: str)`; `Config(boards: list[Board], default: str, poll_s: float = 5.0, log_poll_s: float = 10.0)`; `load_config(path: Path | None = None) -> Config` (default path `~/.config/splitflap/config.toml`; missing file → `Config([], "", …)`; the app then shows a "no config" banner with the expected path). TOML shape:

    ```toml
    default = "leader"
    [[boards]]
    name = "leader"
    url = "http://192.168.15.88"
    [[boards]]
    name = "row0"
    url = "http://192.168.15.121"
    ```
  - `app.py`: `SplitflapApp(App)` with `__init__(config: Config, client_factory: Callable[[str], BoardClient] = BoardClient)` — the factory is the test seam; `DashboardScreen` composing `WallPanel`, `ClusterStrip`, `UnitsTable`, `LogTail`, `StatsBar` (from `widgets.py`) + Textual `Header`/`Footer`.
  - `poller.py`: `Poller(app, client_factory, config)` with thread workers `poll_status()` (every `poll_s`: GET `/status` → `StatusAggregate`, GET `/cluster/status` → `ClusterStatus`; on `Unreachable` set `app.connected = False`, keep retrying — never exit), `poll_log()` (every `log_poll_s`: `fetch_flash_log`, last 200 lines), `sse_loop()` (run `display_events`; on iterator end/`Unreachable`: mark `app.wall_stale = True`, back off 1→2→4→…→30 s, reconnect). All UI updates via `app.call_from_thread`.
  - `widgets.py`: `WallPanel(Static)` with `update_wall(rows: list[str] | None, text: str, stale: bool)` — renders rows one-per-line (STALE suffix in the border title when stale); `ClusterStrip(Static)` with `update_cluster(c: ClusterStatus)` — one line per member: `row{n} {host|self} {rev} joined|SUSPECT|DEGRADED|RESCUE…`; `UnitsTable(DataTable)` with `update_units(h: UnitsHealth)` — columns addr/state/sx/odo/vmin/gates/flags, `—` for `None`; `LogTail(RichLog)`; `StatsBar(Static)` with `update_stats(s: SystemStatsNow, connected: bool)`.
  - `__main__.py`: `main()` — `load_config()`, `SplitflapApp(config).run()`.

- [ ] **Step 1: Write the failing config tests** (`cli/tests/test_config.py`)

```python
from pathlib import Path

from splitflap_tui.config import Board, load_config


def test_load_config(tmp_path: Path):
    p = tmp_path / "config.toml"
    p.write_text('default = "leader"\n'
                 '[[boards]]\nname = "leader"\nurl = "http://10.0.0.2"\n'
                 '[[boards]]\nname = "row0"\nurl = "http://10.0.0.3"\n')
    cfg = load_config(p)
    assert cfg.default == "leader"
    assert cfg.boards[0] == Board("leader", "http://10.0.0.2")
    assert cfg.poll_s == 5.0


def test_missing_file_is_empty_config(tmp_path: Path):
    cfg = load_config(tmp_path / "nope.toml")
    assert cfg.boards == [] and cfg.default == ""
```

- [ ] **Step 2: Run to verify FAIL**, then implement `config.py`:

```python
from __future__ import annotations

import tomllib
from dataclasses import dataclass, field
from pathlib import Path

DEFAULT_PATH = Path.home() / ".config" / "splitflap" / "config.toml"


@dataclass(frozen=True)
class Board:
    name: str
    url: str


@dataclass(frozen=True)
class Config:
    boards: list[Board]
    default: str
    poll_s: float = 5.0
    log_poll_s: float = 10.0

    def board_url(self, name: str = "") -> str:
        wanted = name or self.default
        for b in self.boards:
            if b.name == wanted:
                return b.url
        return ""


def load_config(path: Path | None = None) -> Config:
    path = path or DEFAULT_PATH
    if not path.exists():
        return Config(boards=[], default="")
    data = tomllib.loads(path.read_text())
    boards = [Board(str(b.get("name", "")), str(b.get("url", "")).rstrip("/"))
              for b in data.get("boards", [])]
    return Config(boards=boards, default=str(data.get("default", "")),
                  poll_s=float(data.get("poll_s", 5.0)),
                  log_poll_s=float(data.get("log_poll_s", 10.0)))
```

Run: `cd cli && python -m pytest tests/test_config.py -v` → 2 PASS

- [ ] **Step 3: Write the failing Pilot test** (`cli/tests/test_app.py`)

```python
import httpx
import pytest
from splitflap_client.transport import BoardClient
from splitflap_tui.app import SplitflapApp
from splitflap_tui.config import Board, Config

STATUS = {"settings": {"plat": "esp32s3", "unitCount": 16, "version": "817e3a9",
                       "clusterLeading": True, "deviceMode": "clock"},
          "stats": {"now": {"rssi": -52, "heap": 180000, "minHeap": 150000,
                            "uptime": 3600, "hwm": {"cluster": 2600}}},
          "units": {"width": 16, "faulty": 0,
                    "units": [{"i": 0, "a": 1, "st": 1, "v": 1, "sx": 17}]},
          "cluster": {"enabled": True, "epoch": 7, "seq": 1,
                      "members": [{"host": "", "self": True, "row": 1, "col": 0,
                                   "width": 16, "joined": True,
                                   "degraded": False, "failures": 0,
                                   "rev": "817e3a9", "hmac": True}],
                      "rollout": {"phase": "idle"},
                      "followerImage": {"present": False, "rev": ""},
                      "followerPush": {"phase": "idle", "result": "none"}},
          "ota": {"running": "app0", "next": "app1", "lastInvalid": None,
                  "lastFlashResult": "", "otaReverted": False,
                  "factoryValid": True}}


def fake_factory(url: str) -> BoardClient:
    def handler(req):
        if req.url.path == "/status":
            return httpx.Response(200, json=STATUS)
        if req.url.path == "/cluster/status":
            return httpx.Response(200, json=STATUS["cluster"])
        if req.url.path == "/log/flash":
            return httpx.Response(200, text="hello log\n")
        if req.url.path == "/events":
            return httpx.Response(200, content=b"", 
                                  headers={"content-type": "text/event-stream"})
        return httpx.Response(404, text="nope")
    return BoardClient(url, transport=httpx.MockTransport(handler))


CFG = Config(boards=[Board("leader", "http://leader")], default="leader")


@pytest.mark.asyncio
async def test_dashboard_shows_polled_data():
    app = SplitflapApp(CFG, client_factory=fake_factory)
    async with app.run_test() as pilot:
        await pilot.pause(0.5)
        strip = app.query_one("#cluster-strip")
        assert "817e3a9" in strip.render_str()


@pytest.mark.asyncio
async def test_wall_marks_stale_when_sse_down():
    app = SplitflapApp(CFG, client_factory=fake_factory)
    async with app.run_test() as pilot:
        await pilot.pause(0.5)
        assert app.wall_stale is True     # empty SSE stream ended -> stale
```

Note: `render_str()` — implement on `ClusterStrip` as a plain "current text content" accessor so the test doesn't depend on Textual render internals.

- [ ] **Step 4: Implement `widgets.py`, `poller.py`, `app.py`, `__main__.py`**

`widgets.py` (shapes; keep rendering plain-text so tests assert on strings):

```python
from __future__ import annotations

from textual.widgets import DataTable, RichLog, Static

from splitflap_client.models import ClusterStatus, SystemStatsNow, UnitsHealth


class WallPanel(Static):
    def update_wall(self, rows: list[str] | None, text: str, stale: bool) -> None:
        body = "\n".join(rows) if rows else text
        self.border_title = "wall [STALE]" if stale else "wall"
        self.update(body or "(no display data)")


class ClusterStrip(Static):
    _text: str = ""

    def render_str(self) -> str:
        return self._text

    def update_cluster(self, c: ClusterStatus) -> None:
        lines = []
        for m in c.members:
            flags = [f for f, on in (("SUSPECT", m.suspect),
                                     ("DEGRADED", m.degraded),
                                     ("RESCUE", m.rescue),
                                     ("UPD-BLOCKED", m.update_blocked),
                                     ("STUCK", m.render_stuck)) if on]
            who = "self" if m.self_row else m.host
            state = "joined" if m.joined else f"lost({m.failures})"
            lines.append(f"row{m.row} {who} [{m.plat}] {m.rev} {state} "
                         + (" ".join(flags) if flags else "ok"))
        if c.rollout_phase and c.rollout_phase != "idle":
            lines.append(f"rollout: {c.rollout_phase} src={c.rollout_src or 's3'}")
        self._text = "\n".join(lines) or "(cluster disabled)"
        self.update(self._text)


class UnitsTable(DataTable):
    COLUMNS = ("addr", "st", "sx", "odo", "vmin", "gates", "flags")

    def on_mount(self) -> None:
        self.add_columns(*self.COLUMNS)

    def update_units(self, h: UnitsHealth) -> None:
        def cell(v):
            return "—" if v is None else str(v)
        self.clear()
        for u in h.units:
            flags = "STALE" if u.stale else ("FAULT" if u.fault else "")
            self.add_row(f"0x{u.address:02x}", str(u.state), cell(u.sx),
                         cell(u.odo), cell(u.vcc_min), cell(u.gates), flags)


class LogTail(RichLog):
    pass


class StatsBar(Static):
    def update_stats(self, s: SystemStatsNow, connected: bool) -> None:
        link = "connected" if connected else "DISCONNECTED — retrying"
        self.update(f"{link} | heap {s.heap} (min {s.min_heap}) | "
                    f"rssi {s.rssi} | up {s.uptime}s | "
                    f"i2c {s.i2c_tx}/{s.i2c_err} err")
```

`poller.py`:

```python
from __future__ import annotations

import threading
import time
from typing import Callable

from splitflap_client.events import display_events
from splitflap_client.logs import fetch_flash_log
from splitflap_client.models import ClusterStatus, StatusAggregate
from splitflap_client.transport import BoardClient, SplitflapError


class Poller:
    """Leader-only background polling. Threads, not asyncio: BoardClient is
    sync httpx, and Textual's call_from_thread is the documented bridge."""

    def __init__(self, app, client_factory: Callable[[str], BoardClient],
                 leader_url: str, poll_s: float, log_poll_s: float):
        self.app = app
        self.factory = client_factory
        self.url = leader_url
        self.poll_s = poll_s
        self.log_poll_s = log_poll_s
        self.stop_event = threading.Event()

    def start(self) -> None:
        for fn in (self.poll_status, self.poll_log, self.sse_loop):
            threading.Thread(target=fn, daemon=True).start()

    def stop(self) -> None:
        self.stop_event.set()

    def poll_status(self) -> None:
        while not self.stop_event.is_set():
            try:
                with self.factory(self.url) as c:
                    agg = StatusAggregate.from_json(c.get_json("/status"))
                    cluster = ClusterStatus.from_json(c.get_json("/cluster/status"))
                self.app.call_from_thread(self.app.apply_status, agg, cluster)
            except SplitflapError as exc:
                self.app.call_from_thread(self.app.apply_disconnect, str(exc))
            self.stop_event.wait(self.poll_s)

    def poll_log(self) -> None:
        while not self.stop_event.is_set():
            try:
                with self.factory(self.url) as c:
                    text = fetch_flash_log(c)
                tail = text.splitlines()[-200:]
                self.app.call_from_thread(self.app.apply_log, tail)
            except SplitflapError:
                pass                      # status poller owns the banner
            self.stop_event.wait(self.log_poll_s)

    def sse_loop(self) -> None:
        backoff = 1.0
        while not self.stop_event.is_set():
            try:
                with self.factory(self.url) as c:
                    for event in display_events(c):
                        backoff = 1.0
                        self.app.call_from_thread(self.app.apply_display, event)
            except SplitflapError:
                pass
            self.app.call_from_thread(self.app.apply_wall_stale)
            self.stop_event.wait(backoff)
            backoff = min(backoff * 2, 30.0)
```

`app.py`:

```python
from __future__ import annotations

from typing import Callable

from textual.app import App, ComposeResult
from textual.containers import Horizontal, Vertical
from textual.widgets import Footer, Header

from splitflap_client.events import DisplayEvent
from splitflap_client.models import ClusterStatus, StatusAggregate
from splitflap_client.transport import BoardClient

from .config import Config
from .poller import Poller
from .widgets import ClusterStrip, LogTail, StatsBar, UnitsTable, WallPanel


class SplitflapApp(App):
    TITLE = "splitflap"
    BINDINGS = [("q", "quit", "Quit")]

    def __init__(self, config: Config,
                 client_factory: Callable[[str], BoardClient] = BoardClient):
        super().__init__()
        self.config = config
        self.client_factory = client_factory
        self.connected = False
        self.wall_stale = True
        self.poller: Poller | None = None

    def compose(self) -> ComposeResult:
        yield Header()
        with Vertical():
            yield WallPanel(id="wall")
            yield ClusterStrip(id="cluster-strip")
            with Horizontal():
                yield UnitsTable(id="units")
                yield LogTail(id="log")
            yield StatsBar(id="stats")
        yield Footer()

    def on_mount(self) -> None:
        url = self.config.board_url()
        if not url:
            self.query_one("#stats", StatsBar).update(
                "no config — create ~/.config/splitflap/config.toml")
            return
        self.poller = Poller(self, self.client_factory, url,
                             self.config.poll_s, self.config.log_poll_s)
        self.poller.start()

    # ---- called from poller threads via call_from_thread ----
    def apply_status(self, agg: StatusAggregate, cluster: ClusterStatus) -> None:
        self.connected = True
        self.query_one("#cluster-strip", ClusterStrip).update_cluster(cluster)
        self.query_one("#units", UnitsTable).update_units(agg.units)
        self.query_one("#stats", StatsBar).update_stats(agg.stats_now, True)

    def apply_disconnect(self, message: str) -> None:
        self.connected = False
        stats = self.query_one("#stats", StatsBar)
        stats.update(f"DISCONNECTED — {message} (retrying)")

    def apply_log(self, lines: list[str]) -> None:
        log = self.query_one("#log", LogTail)
        log.clear()
        for line in lines:
            log.write(line)

    def apply_display(self, event: DisplayEvent) -> None:
        self.wall_stale = False
        self.query_one("#wall", WallPanel).update_wall(
            event.rows, event.text, stale=False)

    def apply_wall_stale(self) -> None:
        self.wall_stale = True
        wall = self.query_one("#wall", WallPanel)
        wall.border_title = "wall [STALE]"

    def on_unmount(self) -> None:
        if self.poller:
            self.poller.stop()
```

`__main__.py`:

```python
from .app import SplitflapApp
from .config import load_config


def main() -> None:
    SplitflapApp(load_config()).run()


if __name__ == "__main__":
    main()
```

- [ ] **Step 5: Run the Pilot tests** — `cd cli && python -m pytest tests/test_app.py tests/test_config.py -v`
Expected: 4 PASS (adjust `pilot.pause` duration upward if the thread poll hasn't landed; keep it under 1 s).

- [ ] **Step 6: Manual smoke against the live leader** (VPN): create the real config with leader `http://192.168.15.88`, run `cd cli && python -m splitflap_tui`, confirm: wall shows the clock rows live, cluster strip lists row0/row1, units table fills, `q` quits. This is the bench-verify tier for the read path.

- [ ] **Step 7: Commit**

```bash
git add cli/splitflap_tui/ cli/tests/test_config.py cli/tests/test_app.py
git commit -m "feat(#445,#447): Textual dashboard with leader-only poller, SSE wall, stale marking"
```

---

### Task 12: Command bar — parser, tiers, confirms, execution — #446

**Files:**
- Create: `cli/splitflap_tui/commands.py`, `cli/splitflap_tui/confirm.py`, `cli/tests/test_commands.py`
- Modify: `cli/splitflap_tui/app.py` (command input + execution), `cli/splitflap_tui/widgets.py` (only if a widget hook is needed)

**Interfaces:**
- Consumes: `serves`, `PLAT_S3/PLAT_ESP01`, `control.py` functions, `run_op`, `submit_op`.
- Produces (in `commands.py`, all pure — no I/O):

```python
TIER_KILL = "kill"; TIER_ROUTINE = "routine"; TIER_CONFIRM = "confirm"; TIER_TYPED = "typed"

@dataclass(frozen=True)
class ParsedCommand:
    name: str                 # canonical: stop|text|mode|notify|set|op|gates|reboot|promote|reset-units|addr|cluster-config|cluster-leave
    args: dict                # parsed args, e.g. {"unit": 3} / {"text": "..."}
    tier: str
    route: tuple[str, str]    # (method, path) for capability gating
    summary: str              # human line echoed in the confirm prompt

class CommandError(Exception): ...
def parse(line: str) -> ParsedCommand   # raises CommandError with a usage message
```

  Grammar (space-separated; final free-text arg consumes the rest of the line):
  - `stop` → TIER_KILL, `("POST","/stop")`
  - `text <anything…>` → TIER_ROUTINE (`\n` typed as literal `\n` is passed through — the firmware's grid-row break, #290)
  - `mode text|clock` → TIER_ROUTINE
  - `notify <dwell-seconds> <text…>` → TIER_ROUTINE
  - `set <field> <value>` → TIER_CONFIRM, `("POST","/")`
  - `op home|jog|identify|self-test|reset-odometer|offset <unit> [value]` → TIER_CONFIRM (`jog` requires steps value, `offset` requires value)
  - `gates <unit> <mask>` → TIER_CONFIRM
  - `reboot` → TIER_TYPED, `("POST","/reboot")`
  - `reset-units` → TIER_TYPED
  - `addr burn <unit> <target> | addr clear <unit>` → TIER_TYPED (param names from the Task 7 Step 5 note)
  - `promote` → TIER_TYPED, `("POST","/cluster/promote")`
  - `cluster leave` → TIER_TYPED
  - Execution dispatch `execute(parsed, client) -> str` lives in `app.py` (it needs the client + op polling); it maps op names to routes (`op home 3` → `run_op(client, "/unit/home", {"address": 3})`) and formats `OpResult`/`HttpError.body` into the status line.
- `confirm.py`: `ConfirmModal(ModalScreen[bool])` — TIER_CONFIRM: any-key y/n; TIER_TYPED: requires typing the echoed token (unit address or command name) exactly.

- [ ] **Step 1: Write the failing parser tests** (`cli/tests/test_commands.py`)

```python
import pytest
from splitflap_tui.commands import (CommandError, TIER_CONFIRM, TIER_KILL,
                                    TIER_ROUTINE, TIER_TYPED, parse)


def test_stop_is_kill_tier():
    c = parse("stop")
    assert c.tier == TIER_KILL and c.route == ("POST", "/stop")


def test_text_consumes_rest_of_line():
    c = parse("text ICE 704  +5")
    assert c.args["text"] == "ICE 704  +5" and c.tier == TIER_ROUTINE


def test_notify_dwell_and_text():
    c = parse("notify 15 DOOR OPEN")
    assert c.args == {"dwell": 15, "text": "DOOR OPEN"}


def test_op_home_requires_unit():
    c = parse("op home 3")
    assert c.args["unit"] == 3 and c.route == ("POST", "/unit/home")
    with pytest.raises(CommandError):
        parse("op home")


def test_op_jog_requires_value():
    assert parse("op jog 3 -10").args == {"unit": 3, "value": -10}
    with pytest.raises(CommandError):
        parse("op jog 3")


def test_dangerous_tiers():
    assert parse("reboot").tier == TIER_TYPED
    assert parse("reset-units").tier == TIER_TYPED
    assert parse("addr burn 3 7").tier == TIER_TYPED
    assert parse("cluster leave").tier == TIER_TYPED


def test_unknown_command_raises_usage():
    with pytest.raises(CommandError):
        parse("frobnicate")
```

- [ ] **Step 2: Run to verify FAIL**, then implement `commands.py` — a table-driven parser:

```python
from __future__ import annotations

from dataclasses import dataclass

TIER_KILL = "kill"
TIER_ROUTINE = "routine"
TIER_CONFIRM = "confirm"
TIER_TYPED = "typed"


class CommandError(Exception):
    pass


@dataclass(frozen=True)
class ParsedCommand:
    name: str
    args: dict
    tier: str
    route: tuple[str, str]
    summary: str


OP_ROUTES = {"home": "/unit/home", "jog": "/unit/jog",
             "identify": "/unit/identify", "self-test": "/unit/self-test",
             "reset-odometer": "/unit/reset-odometer", "offset": "/unit/offset"}
OPS_NEED_VALUE = {"jog", "offset"}


def _int_arg(tokens: list[str], index: int, what: str) -> int:
    try:
        return int(tokens[index], 0)
    except (IndexError, ValueError):
        raise CommandError(f"usage: expected {what}") from None


def parse(line: str) -> ParsedCommand:
    tokens = line.strip().split()
    if not tokens:
        raise CommandError("empty command")
    head, rest = tokens[0], tokens[1:]

    if head == "stop":
        return ParsedCommand("stop", {}, TIER_KILL, ("POST", "/stop"),
                             "STOP — blank and halt the wall")
    if head == "text":
        text = line.strip()[len("text "):] if rest else ""
        if not text:
            raise CommandError("usage: text <display text>")
        return ParsedCommand("text", {"text": text}, TIER_ROUTINE,
                             ("POST", "/"), f"set text: {text!r}")
    if head == "mode":
        if rest[:1] not in (["text"], ["clock"]):
            raise CommandError("usage: mode text|clock")
        return ParsedCommand("mode", {"mode": rest[0]}, TIER_ROUTINE,
                             ("POST", "/"), f"mode {rest[0]}")
    if head == "notify":
        dwell = _int_arg(rest, 0, "dwell seconds")
        text = " ".join(rest[1:])
        if not text:
            raise CommandError("usage: notify <dwell-s> <text>")
        return ParsedCommand("notify", {"dwell": dwell, "text": text},
                             TIER_ROUTINE, ("POST", "/"),
                             f"notify {dwell}s: {text!r}")
    if head == "set":
        if len(rest) < 2:
            raise CommandError("usage: set <field> <value>")
        return ParsedCommand("set", {"field": rest[0],
                                     "value": " ".join(rest[1:])},
                             TIER_CONFIRM, ("POST", "/"),
                             f"set {rest[0]} = {' '.join(rest[1:])}")
    if head == "op":
        if not rest or rest[0] not in OP_ROUTES:
            raise CommandError(f"usage: op {'|'.join(OP_ROUTES)} <unit> [value]")
        op = rest[0]
        unit = _int_arg(rest, 1, "unit address")
        args: dict = {"unit": unit}
        if op in OPS_NEED_VALUE:
            args["value"] = _int_arg(rest, 2, f"{op} value")
        return ParsedCommand("op", dict(args, op=op), TIER_CONFIRM,
                             ("POST", OP_ROUTES[op]), f"op {op} unit {unit}")
    if head == "gates":
        unit = _int_arg(rest, 0, "unit address")
        mask = _int_arg(rest, 1, "gate mask")
        return ParsedCommand("gates", {"unit": unit, "mask": mask},
                             TIER_CONFIRM, ("POST", "/unit/gates"),
                             f"gates unit {unit} mask 0x{mask:02x}")
    if head == "reboot":
        return ParsedCommand("reboot", {}, TIER_TYPED, ("POST", "/reboot"),
                             "REBOOT the board")
    if head == "reset-units":
        return ParsedCommand("reset-units", {}, TIER_TYPED,
                             ("POST", "/reset-units"), "RESET every unit")
    if head == "addr":
        if rest[:1] == ["burn"]:
            unit = _int_arg(rest, 1, "unit address")
            target = _int_arg(rest, 2, "target address")
            return ParsedCommand("addr", {"mode": "burn", "unit": unit,
                                          "target": target}, TIER_TYPED,
                                 ("POST", "/unit/set-address"),
                                 f"BURN address {unit} -> {target}")
        if rest[:1] == ["clear"]:
            unit = _int_arg(rest, 1, "unit address")
            return ParsedCommand("addr", {"mode": "clear", "unit": unit},
                                 TIER_TYPED, ("POST", "/unit/clear-address"),
                                 f"CLEAR address of unit {unit}")
        raise CommandError("usage: addr burn <unit> <target> | addr clear <unit>")
    if head == "promote":
        return ParsedCommand("promote", {}, TIER_TYPED,
                             ("POST", "/cluster/promote"),
                             "PROMOTE this board to leader")
    if head == "cluster" and rest[:1] == ["leave"]:
        return ParsedCommand("cluster-leave", {}, TIER_TYPED,
                             ("POST", "/cluster/leave"), "LEAVE the cluster")
    raise CommandError(f"unknown command: {head}")
```

Run: `cd cli && python -m pytest tests/test_commands.py -v` → 7 PASS

- [ ] **Step 3: Wire into the app** — add to `app.py`: an `Input(id="command")` toggled by the `:` binding; on `Input.Submitted` → `parse`; `CommandError` → status line; TIER_ROUTINE/KILL execute immediately, TIER_CONFIRM pushes `ConfirmModal(summary, typed=False)`, TIER_TYPED pushes `ConfirmModal(summary, typed=True, token=str(args.get("unit", parsed.name)))`. `execute(parsed, client)` maps: `stop`→`control.stop`, `text`→`set_text`, `mode`→`set_mode`, `notify`→`notify`, `set`→`set_setting`, `op`→`run_op` with `{"address": unit}` (+`value`/`steps` per op: offset sends `value`, jog sends `steps`), `gates`→`run_op("/unit/gates", {"address": unit, "gates": mask})`, `reboot`→`control.reboot`, others→`client.post(path)`. Every `HttpError` renders as `⛔ {status}: {body}` (verbatim); `OpResult` renders as `state[, reason[, detail]]`. Execution runs in a thread worker (`self.run_worker(..., thread=True)`) so the UI never blocks on an op poll. `confirm.py`:

```python
from __future__ import annotations

from textual.app import ComposeResult
from textual.containers import Vertical
from textual.screen import ModalScreen
from textual.widgets import Input, Label


class ConfirmModal(ModalScreen[bool]):
    BINDINGS = [("y", "yes", "confirm"), ("n", "no", "cancel"),
                ("escape", "no", "cancel")]

    def __init__(self, summary: str, typed: bool = False, token: str = ""):
        super().__init__()
        self.summary = summary
        self.typed = typed
        self.token = token

    def compose(self) -> ComposeResult:
        with Vertical(id="confirm-box"):
            yield Label(self.summary)
            if self.typed:
                yield Label(f"type '{self.token}' to confirm, Esc to cancel")
                yield Input(id="confirm-input")
            else:
                yield Label("y to confirm, n/Esc to cancel")

    def action_yes(self) -> None:
        if not self.typed:
            self.dismiss(True)

    def action_no(self) -> None:
        self.dismiss(False)

    def on_input_submitted(self, event: Input.Submitted) -> None:
        self.dismiss(event.value.strip() == self.token)
```

- [ ] **Step 4: Pilot test the flows** — append to `test_commands.py`:

```python
import httpx
import pytest
from splitflap_client.transport import BoardClient
from splitflap_tui.app import SplitflapApp
from splitflap_tui.config import Board, Config


@pytest.mark.asyncio
async def test_routine_command_executes_without_confirm():
    posts = []
    def handler(req):
        if req.method == "POST":
            posts.append((req.url.path, req.content.decode()))
            return httpx.Response(200, text="ok")
        return httpx.Response(200, json={})
    cfg = Config(boards=[Board("leader", "http://x")], default="leader")
    app = SplitflapApp(cfg, client_factory=lambda url: BoardClient(
        url, transport=httpx.MockTransport(handler)))
    async with app.run_test() as pilot:
        await pilot.press(":")
        await pilot.press(*"text HI")
        await pilot.press("enter")
        await pilot.pause(0.3)
    assert any(path == "/" and "inputText=HI" in body for path, body in posts)


@pytest.mark.asyncio
async def test_typed_confirm_blocks_reboot_until_token():
    posts = []
    def handler(req):
        if req.method == "POST":
            posts.append(req.url.path)
            return httpx.Response(200, text="rebooting")
        return httpx.Response(200, json={})
    cfg = Config(boards=[Board("leader", "http://x")], default="leader")
    app = SplitflapApp(cfg, client_factory=lambda url: BoardClient(
        url, transport=httpx.MockTransport(handler)))
    async with app.run_test() as pilot:
        await pilot.press(":")
        await pilot.press(*"reboot")
        await pilot.press("enter")
        await pilot.pause(0.2)
        assert "/reboot" not in posts          # modal is up, nothing sent
        await pilot.press("escape")            # cancel
        await pilot.pause(0.2)
    assert "/reboot" not in posts
```

Run: `cd cli && python -m pytest tests/test_commands.py -v` → all PASS

- [ ] **Step 5: Manual smoke on the live leader** — `text`, `mode clock`, `notify 10 TEST`, a `stop` and a re-`mode clock`; verify a deliberate 409 (send `text` while row-clustered board — expect verbatim `clustered`). Restore `deviceMode=clock` after.

- [ ] **Step 6: Commit**

```bash
git add cli/splitflap_tui/commands.py cli/splitflap_tui/confirm.py cli/splitflap_tui/app.py cli/tests/test_commands.py
git commit -m "feat(#446): command bar with tiered confirms, verbatim 409 surfacing, stop kill switch"
```

---

### Task 13: Board detail + log screens, packaging, CI — #447

**Files:**
- Create: `cli/splitflap_tui/screens/__init__.py`, `cli/splitflap_tui/screens/board_detail.py`, `cli/splitflap_tui/screens/log_screen.py`, `cli/README.md`, `cli/tests/test_screens.py`
- Modify: `cli/splitflap_tui/app.py` (bindings `b` → board picker, `l` → log screen), `.github/workflows/build.yml` (add cli job), `CLAUDE.md` (one repository-map line for `cli/`)

**Interfaces:**
- Consumes: everything prior; `serves`/`plat_from_settings` for per-platform rendering; `fetch_follower_log` for esp01.
- Produces: `BoardDetailScreen(Screen)` — `__init__(board: Board, client_factory)`; on mount starts ONE thread worker polling that board's `/settings` + `/units/health` + `/cluster/health` every 5 s (≥5 s floor: never faster against an esp01) and, for esp01, `fetch_follower_log` with a kept cursor; renders only capability-served sections; `escape` pops the screen AND stops the worker (the follower is polled only while this screen is open). `LogScreen(Screen)` — full-height `RichLog` of the leader flash log, binding `p` toggles `prev` (re-fetch with `?prev=1`), `escape` pops.

- [ ] **Step 1: Write the failing Pilot test** (`cli/tests/test_screens.py`)

```python
import httpx
import pytest
from splitflap_client.transport import BoardClient
from splitflap_tui.app import SplitflapApp
from splitflap_tui.config import Board, Config
from splitflap_tui.screens.board_detail import BoardDetailScreen

ESP01_SETTINGS = {"plat": "esp01", "width": 5, "version": "9f694dd",
                  "clusterState": "clustered", "effectiveDeviceName": "row0"}


@pytest.mark.asyncio
async def test_board_detail_esp01_polls_only_while_open():
    calls = []
    def handler(req):
        calls.append(req.url.path)
        if req.url.path == "/settings":
            return httpx.Response(200, json=ESP01_SETTINGS)
        if req.url.path == "/units/health":
            return httpx.Response(200, json={"width": 5, "faulty": 0, "units": []})
        if req.url.path == "/cluster/health":
            return httpx.Response(200, json={"state": "clustered", "rev": "9f694dd",
                                             "hmac": True,
                                             "foreign": {"joins": 0, "pings": 0,
                                                         "renders": 0,
                                                         "lastHost": "",
                                                         "msSince": -1}})
        if req.url.path == "/log":
            return httpx.Response(200, text="10\nhello\n")
        return httpx.Response(404, text="nope")
    factory = lambda url: BoardClient(url, transport=httpx.MockTransport(handler))
    cfg = Config(boards=[Board("row0", "http://row0")], default="row0")
    app = SplitflapApp(cfg, client_factory=factory)
    async with app.run_test() as pilot:
        app.push_screen(BoardDetailScreen(cfg.boards[0], factory))
        await pilot.pause(0.5)
        assert "/settings" in calls and "/log" in calls
        n_before = len(calls)
        await pilot.press("escape")
        await pilot.pause(0.6)
        assert len(calls) == n_before       # no polling after close
```

- [ ] **Step 2: Implement the two screens.** `board_detail.py` core shape:

```python
from __future__ import annotations

import threading
from typing import Callable

from textual.app import ComposeResult
from textual.screen import Screen
from textual.widgets import Footer, Header, Static

from splitflap_client.capability import PLAT_ESP01
from splitflap_client.logs import fetch_follower_log
from splitflap_client.models import ClusterHealth, Settings, UnitsHealth
from splitflap_client.transport import BoardClient, SplitflapError

from ..config import Board

POLL_S = 5.0        # floor — never poll a follower faster (esp01 superloop)


class BoardDetailScreen(Screen):
    BINDINGS = [("escape", "app.pop_screen", "back")]

    def __init__(self, board: Board, client_factory: Callable[[str], BoardClient]):
        super().__init__()
        self.board = board
        self.factory = client_factory
        self.stop_event = threading.Event()
        self.log_cursor = 0

    def compose(self) -> ComposeResult:
        yield Header()
        yield Static(id="detail-settings")
        yield Static(id="detail-units")
        yield Static(id="detail-cluster")
        yield Static(id="detail-log")
        yield Footer()

    def on_mount(self) -> None:
        threading.Thread(target=self._poll, daemon=True).start()

    def on_unmount(self) -> None:
        self.stop_event.set()

    def _poll(self) -> None:
        while not self.stop_event.is_set():
            try:
                with self.factory(self.board.url) as c:
                    settings = Settings.from_json(c.get_json("/settings"))
                    units = UnitsHealth.from_json(c.get_json("/units/health"))
                    health = ClusterHealth.from_json(c.get_json("/cluster/health"))
                    log_text = None
                    if settings.plat == PLAT_ESP01:
                        delta = fetch_follower_log(c, after=self.log_cursor)
                        self.log_cursor = delta.cursor
                        log_text = delta.text
                self.app.call_from_thread(self._apply, settings, units, health,
                                          log_text)
            except SplitflapError as exc:
                self.app.call_from_thread(self._apply_error, str(exc))
            self.stop_event.wait(POLL_S)

    def _apply(self, settings: Settings, units: UnitsHealth,
               health: ClusterHealth, log_text: str | None) -> None:
        self.query_one("#detail-settings", Static).update(
            f"{self.board.name} [{settings.plat}] rev {settings.version} "
            f"mode={settings.device_mode or '-'} state={settings.cluster_state} "
            f"heap={settings.heap} rssi={settings.rssi} up={settings.up}s")
        faults = ", ".join(f"0x{u.address:02x}" for u in units.units if u.fault)
        self.query_one("#detail-units", Static).update(
            f"units {units.width} faulty {units.faulty}"
            + (f" [{faults}]" if faults else ""))
        extra = f" stackFree={health.stack_free}" if health.stack_free is not None else ""
        self.query_one("#detail-cluster", Static).update(
            f"cluster {health.state} hmac={'on' if health.hmac else 'off'} "
            f"foreign j/p/r {health.foreign_joins}/{health.foreign_pings}/"
            f"{health.foreign_renders}{extra}")
        if log_text:
            widget = self.query_one("#detail-log", Static)
            widget.update(str(widget.renderable) + log_text)

    def _apply_error(self, message: str) -> None:
        self.query_one("#detail-settings", Static).update(
            f"{self.board.name}: UNREACHABLE — {message}")
```

`log_screen.py`: a `Screen` with one `RichLog`, `on_mount` fetches `fetch_flash_log(client, prev=self.prev)` in a thread worker, binding `p` flips `self.prev` and refetches, `escape` pops. Wire both into `app.py`: binding `l` pushes `LogScreen`, binding `b` pushes `BoardDetailScreen` for the next board in `config.boards` (cycle on repeat press; status line names it).

- [ ] **Step 3: Run all tests** — `cd cli && python -m pytest tests/ -v`
Expected: everything PASS.

- [ ] **Step 4: CI job** — in `.github/workflows/build.yml`, alongside the existing pytest jobs, add:

```yaml
  cli-tests:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - uses: actions/setup-python@v5
        with:
          python-version: "3.11"
      - run: pip install -e "./cli[dev]"
      - run: python -m pytest cli/tests/ -v
```

(Match the file's existing job style — copy the checkout/python steps from the neighbouring pytest job verbatim if they differ from the above.)

- [ ] **Step 5: README + CLAUDE.md line.** `cli/README.md`: install (`pipx install ./cli`), config file example (the TOML from Task 11), keybindings (`:` command, `b` board, `l` log, `q` quit), command grammar table (from Task 12), the v1 cut line ("never moves firmware images — use ota-flash.sh"). In `CLAUDE.md` repository map add one line: `- \`cli/\` — operator TUI (\`splitflap\`, #441): Textual dashboard + command bar over the HTTP surface; \`splitflap_client\` is the typed client library (capability table drift-gated against /api fixtures). Tests: \`cd cli && python -m pytest tests/\`.`

- [ ] **Step 6: Full-suite + bench verify.** `cd cli && python -m pytest tests/ -v` green; live smoke: dashboard up against the leader, `b` into row0 (esp01) — settings/units/foreign counters render, no poll after leaving the screen (watch `/log` cursor traffic stop); `l` shows the flash log, `p` flips to prev boot.

- [ ] **Step 7: Commit**

```bash
git add cli/ .github/workflows/build.yml CLAUDE.md
git commit -m "feat(#447): board-detail + log screens, cli CI job, README"
```

---

### Task 14: Final gate — review, PR, issue closure

- [ ] **Step 1:** Run the full repo test surface that this arc touches: `cd cli && python -m pytest tests/ -v` (the firmware suites are untouched — no firmware file changed; if CI runs them anyway they must stay green).
- [ ] **Step 2:** Batched review per the project workflow: python-reviewer (or code-reviewer) over the combined branch diff `git diff master...HEAD`. No cluster-wire/OTA/flash code changed, so the always-review tier is not triggered, but the batch gate still applies. Address CRITICAL/HIGH findings.
- [ ] **Step 3:** Push and open the PR:

```bash
git push -u origin feat/splitflap-tui-spec
gh pr create --title "splitflap operator TUI (epic #441)" --body "$(cat <<'EOF'
One interactive Textual program for operating the wall — spec docs/superpowers/specs/2026-08-08-splitflap-tui-design.md, plan docs/superpowers/plans/2026-08-08-splitflap-tui.md.

- splitflap_client: transport with verbatim-body errors, tolerant typed models, per-plat capability table drift-gated against committed /api fixtures, seq-op state machine, SSE consumer, log access (flash log + esp01 ring cursor)
- splitflap_tui: leader-only dashboard (SSE wall with STALE marking, cluster strip, units, log tail, stats), command bar with 4 confirm tiers and a stop kill switch, board-detail + log screens
- cli CI job; fixtures captured from the live boards

Closes #442
Closes #443
Closes #444
Closes #445
Closes #446
Closes #447
EOF
)"
```

  (Epic #441 stays open until the TUI has carried a full real operations session.)
- [ ] **Step 4:** Update memory (`MEMORY.md` + a project file): what shipped, the wire facts worth keeping (form POST `/` contract, op param names, esp01 log cursor), and the v2 backlog (OTA absorption, member/update, `--json` one-shot mode, feeder daemon brainstorm).

---

## Self-Review (done at plan-writing time)

- **Spec coverage:** transport/models/capability/ops/SSE (spec "Client library") → Tasks 1-9; dashboard panels + refresh + failure rules → Task 11; command bar tiers + inventory → Task 12; board detail/log screens + config + packaging + CI → Task 13; wire-twin + fixture testing strategy → Tasks 2, 5, 10; "leader-only polling" enforced structurally in Task 11 (poller only ever gets the leader URL) and Task 13 (detail-screen worker stops on unmount, 5 s floor). Deliberate v1 exclusions (OTA, reflash campaigns, member/update, rescue/coredump management beyond read-only summary) appear in no task — consistent with the spec.
- **Known deviations from spec text:** (1) the spec's cluster-strip "esp01 heap/RSSI" moved to the board-detail screen — `/cluster/status` members don't carry heap/rssi (ClusterDigest.h:79-190); the strip shows what the leader actually knows. (2) The spec's `notify` command is implemented as the firmware's `transientText`/`transientDwell` form pair — there is no notify route. (3) `promote` targets the local board's `/cluster/promote` (only meaningful on a LocalFallback S3 follower; the firmware answers with its own verdict text either way).
- **Type consistency:** `BoardClient(base_url, *, timeout, transport)` used identically in every task; `Settings.from_json`/`UnitsHealth.from_json`/`ClusterStatus.from_json`/`StatusAggregate.from_json` names consistent across Tasks 3-5, 10-13; `OpResult(state, reason, detail)` consistent between Tasks 7 and 12; `Config`/`Board`/`board_url` consistent between Tasks 11-13; `client_factory: Callable[[str], BoardClient]` is the single injection seam everywhere.
- **Placeholder scan:** the two deliberately deferred verifications are concrete read-and-mirror steps with exact file:line targets (Task 7 Step 5: set-address param names at WebMaintenance.cpp:305-338; Task 10 Step 2: fake_follower argparse) — each lands inside the task that needs it, with the fallback stated.
