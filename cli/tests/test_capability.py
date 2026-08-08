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
