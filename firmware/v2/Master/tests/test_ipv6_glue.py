"""Source gate for IPv6 on the master (#612).

LanOrigin.h decides what an IPv6 origin means (natively tested, Rescue's
test_rescue_cors). What a native test cannot reach: the interface gets IPv6
before it joins, an arrived address is noticed, the own row's delegated
name carries the IPv6 addresses too, and the board's facts list them.
"""
import re
from pathlib import Path

MASTER = Path(__file__).resolve().parent.parent


def _code(name):
    return re.sub(r"//[^\n]*", "", (MASTER / name).read_text())


def test_ipv6_is_enabled_on_the_station_before_it_joins():
    code = _code("WifiService.cpp")
    assert code.index("WiFi.mode(WIFI_STA);") < code.index("WiFi.enableIPv6();") < code.index("WiFi.begin(")
    assert "ARDUINO_EVENT_WIFI_STA_GOT_IP6" in code
    assert "ESP_IPADDR_TYPE_V6" in code[code.index("static void ownRowNameTick()"):]


def test_the_board_facts_list_the_addresses():
    code = _code("WebBoard.cpp")
    master = code[code.index("void writeMaster("):]
    assert 'root["ipv6"]' in master and "wifiIpv6Global()" in master
