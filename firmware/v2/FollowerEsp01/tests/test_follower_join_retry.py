"""Source gate for the ESP-01 follower's WiFi join retry (#524).

The join loop is radio glue no native test reaches. One failed association
used to send the row straight to the setup portal for five minutes; this pins
the loop that tries again first, and the two things it must never do.
"""
import re
from pathlib import Path

PROJECT = Path(__file__).resolve().parent.parent


def _code(name):
    text = (PROJECT / name).read_text()
    return re.sub(r"//[^\n]*", "", text)


def _wifi_init():
    code = _code("FollowerWifi.cpp")
    start = code.index("void wifiInit(")
    return code[start:code.index("void wifiServicesInit(")]


def test_join_is_tried_several_times_before_the_portal():
    body = _wifi_init()
    loop = body.index("for (int attempt = 1; attempt <= FOLLOWER_JOIN_ATTEMPTS; attempt++)")
    join = body.index("tryJoinKnownWifi(FOLLOWER_JOIN_WINDOW_S)", loop)
    portal = body.index("startConfigPortal(")
    assert loop < join < portal
    # The only join call is the one inside the loop.
    assert body.count("tryJoinKnownWifi(") == 1


def test_attempt_count_and_window_are_the_documented_ones():
    config = _code("FollowerConfig.h")
    assert re.search(r"#define\s+FOLLOWER_JOIN_ATTEMPTS\s+3\b", config)
    assert re.search(r"#define\s+FOLLOWER_JOIN_WINDOW_S\s+30\b", config)


def test_no_stored_credentials_go_straight_to_the_portal():
    body = _wifi_init()
    loop = body.index("for (int attempt = 1;")
    assert "if (WiFi.SSID().length() == 0) break;" in body[loop:body.index("startConfigPortal(")]


def test_retry_never_disconnects():
    # WiFi.disconnect() can erase the SDK-persisted credentials on this core.
    assert "WiFi.disconnect(" not in _code("FollowerWifi.cpp")
