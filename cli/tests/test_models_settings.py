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
