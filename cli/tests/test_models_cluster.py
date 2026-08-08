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
