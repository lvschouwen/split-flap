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
