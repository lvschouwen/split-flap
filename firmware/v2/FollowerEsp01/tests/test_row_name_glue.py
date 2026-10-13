"""Source gate for the row's hostname (#611): the name the master gives the
row's place is what DHCP and mDNS answer to, the identity stays what the
link, the pairing and a master's scan know the board by, and a changed name
is stored, announced and carried to the router by a reconnect. The record
itself is FollowerSettings.h, natively tested."""
import re
from pathlib import Path

TREE = Path(__file__).resolve().parent.parent


def _code(name: str) -> str:
    return re.sub(r"//[^\n]*", "", (TREE / name).read_text())


def test_the_hostname_is_what_dhcp_and_mdns_take_and_the_identity_stays_in_the_txt() -> None:
    wifi = _code("FollowerWifi.cpp")
    assert "WiFi.hostname(hostName.c_str());" in wifi
    assert "WiFi.hostname(effectiveDeviceName" not in wifi
    assert "MDNS.begin(hostName.c_str())" in wifi
    assert re.search(r'MDNS\.addServiceTxt\("splitflap", "tcp", "name",\s*effectiveDeviceName', wifi)
    # The stored name is read before the first join, so the first DHCP
    # request already carries it.
    init = wifi[wifi.index("void wifiInit("):]
    assert init.index("hostName = hostNameFor(clusterViewGet().hostName);") < init.index("tryJoinKnownWifi(")


def test_a_changed_name_is_announced_and_carried_to_the_router_by_a_reconnect() -> None:
    wifi = _code("FollowerWifi.cpp")
    body = wifi[wifi.index("void followerWifiSetHostName("):]
    body = body[:body.index("\n}\n")]
    assert "if (next == hostName) return;" in body
    assert "MDNS.setHostname(hostName.c_str());" in body
    assert "WiFi.reconnect();" in body
    assert "WiFi.disconnect(" not in body  # can erase the stored credentials


def test_the_link_stores_the_name_first_and_release_takes_it_back() -> None:
    link = _code("FollowerLink.cpp")
    name = link[link.index("case wl_ToRow_name_tag:"):]
    name = name[:name.index("case wl_ToRow_release_tag:")]
    assert "if (clusterSetHostName(host)) followerWifiSetHostName(host);" in name
    release = link[link.index("case wl_ToRow_release_tag:"):]
    release = release[:release.index("break;")]
    assert release.index("clusterHandleLeave();") < release.index('followerWifiSetHostName("");')


def test_the_name_is_part_of_the_pairing_record() -> None:
    cluster = _code("FollowerCluster.cpp")
    assert "followerMembershipHostname(blob, hostname);" in cluster
    assert "givenHostName.c_str())" in cluster[cluster.index("static void persistMembership()"):]
    leave = cluster[cluster.index("void clusterHandleLeave()"):]
    assert 'givenHostName = "";' in leave[:leave.index("\n}\n")]
