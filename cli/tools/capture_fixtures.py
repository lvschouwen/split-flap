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
