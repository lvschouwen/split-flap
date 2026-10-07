"""The routes the row board serves are exactly the ones it needs without a
master. Everything else reaches it over the wall link, so a route added here
is a second way in that nothing else knows about.
"""
import re
from pathlib import Path

TREE = Path(__file__).resolve().parents[1]
ROUTE = re.compile(r'server\.on\(\s*"([^"]+)"\s*,\s*(HTTP_[A-Z]+)')


def _code(name):
    src = (TREE / name).read_text()
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.DOTALL)
    return re.sub(r"(?m)^\s*//[^\n]*$", "", src)


def test_the_row_serves_three_routes():
    served = set(ROUTE.findall(_code("FollowerWeb.cpp")))
    assert served == {("/firmware/master", "HTTP_POST"),
                      ("/settings", "HTTP_GET"),
                      ("/pair", "HTTP_POST")}


def test_no_other_file_registers_a_route():
    for path in sorted(TREE.glob("*.cpp")):
        if path.name == "FollowerWeb.cpp":
            continue
        assert not ROUTE.search(_code(path.name)), path.name


def test_rescue_mode_serves_the_same_routes():
    main = _code("main.cpp")
    setup = main[main.index("void setup()"):main.index("void loop()")]
    register = setup.index("webEndpointsInit(webServer);")
    # Registered outside every rescue branch.
    before = setup[:register]
    assert before.count("{") - before.count("}") == 1
    init = _code("FollowerWeb.cpp")
    init = init[init.index("void webEndpointsInit("):]
    assert "rescueActive()" not in init[:init.index("\n}\n")]


def test_every_post_route_refuses_a_website():
    web = _code("FollowerWeb.cpp")
    pair = web[web.index('server.on("/pair"'):]
    assert pair.index("followerRejectCsrf(request)") < pair.index("clusterPair(")
    upload = web[web.index('server.on("/firmware/master"'):web.index("void webEndpointsInit(")]
    assert upload.index("lanCsrfReject(") < upload.index("Update.begin(")


def test_pairing_goes_through_the_policy_and_loop_writes_the_record():
    web = _code("FollowerWeb.cpp")
    pair = web[web.index('server.on("/pair"'):]
    assert pair.index("followerPairDecide(") < pair.index("clusterPair(")
    # Only a Store verdict changes anything.
    assert pair.count("clusterPair(") == 1
    stored = pair.index("case PairVerdict::Store:")
    assert pair[stored:pair.index("break;", stored)].count("clusterPair(master, caller);") == 1
    assert "EEPROM" not in web
    cluster = _code("FollowerCluster.cpp")
    fn = cluster[cluster.index("void clusterPair("):]
    fn = fn[:fn.index("\n}\n")]
    assert "membershipDirty = true;" in fn and "EEPROM" not in fn


def test_a_changed_pairing_closes_the_open_connection():
    link = _code("FollowerLink.cpp")
    tick = link[link.index("void linkLoopTick()"):]
    changed = tick.index('drop(F("paired with another master"));')
    assert changed < tick.index("sock.connect(host, WALL_LINK_PORT)")
    assert changed < tick.index("sock.available()"), "before anything is read from it"
    dial = tick[tick.index("sock.connect(host, WALL_LINK_PORT)"):tick.index("sendHello();")]
    assert "strlcpy(dialledId," in dial and "strlcpy(dialledHost," in dial


def test_a_log_line_that_did_not_get_out_is_kept():
    link = _code("FollowerLink.cpp")
    tick = link[link.index("void logTick()"):]
    tick = tick[:tick.index("\n}\n")]
    assert "nextLine(next," in tick and "if (send()) logCursor = next;" in tick


def test_the_log_goes_up_only_while_the_master_asks():
    link = _code("FollowerLink.cpp")
    tick = link[link.index("void logTick()"):]
    tick = tick[:tick.index("\n}\n")]
    assert tick.index("if (!logOn) return;") < tick.index("nextLine(")
    drop = link[link.index("void drop("):]
    assert "logOn = false;" in drop[:drop.index("\n}\n")]
