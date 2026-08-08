import pytest
from splitflap_tui.commands import (CANONICAL_NAMES, CommandError, HELP,
                                    TIER_CONFIRM, TIER_KILL, TIER_ROUTINE,
                                    TIER_TYPED, parse)


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


def test_reboot_accepts_optional_board_arg():
    """#441 finding 4: the spec's dangerous tier is `reboot [board]`, not a
    fixed target — the board name (or its absence) rides in args."""
    no_board = parse("reboot")
    assert no_board.tier == TIER_TYPED and no_board.args == {"board": ""}

    with_board = parse("reboot row0")
    assert with_board.tier == TIER_TYPED
    assert with_board.args == {"board": "row0"}
    assert with_board.route == ("POST", "/reboot")


def test_unknown_command_raises_usage():
    with pytest.raises(CommandError):
        parse("frobnicate")


def test_cluster_config_is_typed_tier_with_verified_members_form_param():
    # VERIFIED against WebCluster.cpp:483-491 (fix round 1, item 4): POST
    # /cluster/config takes a "members" FORM param — see app.py's execute().
    c = parse("cluster config host1|0|0|16;host2|1|0|5")
    assert c.tier == TIER_TYPED
    assert c.route == ("POST", "/cluster/config")
    assert c.args == {"members": "host1|0|0|16;host2|1|0|5"}
    assert c.name == "config"          # so the typed-confirm token is "config"
    with pytest.raises(CommandError):
        parse("cluster config")


import httpx
from textual.widgets import Input, Static

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


# ---- fix round 1 (#446 review): capability gating, stop key, history, cluster config

from splitflap_client.capability import PLAT_ESP01


@pytest.mark.asyncio
async def test_capability_gate_rejects_route_not_served_on_current_plat():
    """Item 1(a): /stop is documented on PLAT_S3's CLIENT_ROUTES but not
    PLAT_ESP01's — on an esp01 board it must be rejected client-side, before
    any request goes out, naming the current plat on the status line."""
    posts = []
    def handler(req):
        if req.method == "POST":
            posts.append(req.url.path)
        return httpx.Response(200, json={})
    cfg = Config(boards=[Board("leader", "http://x")], default="leader")
    app = SplitflapApp(cfg, client_factory=lambda url: BoardClient(
        url, transport=httpx.MockTransport(handler)))
    async with app.run_test() as pilot:
        app.plat = PLAT_ESP01
        await pilot.press(":")
        await pilot.press(*"stop")
        await pilot.press("enter")
        await pilot.pause(0.2)
        status = app.query_one("#cmd-status", Static)
        assert "not served on esp01" in status.content
    assert "/stop" not in posts


@pytest.mark.asyncio
async def test_cluster_leave_still_dispatches_on_s3():
    """Item 1(b): /cluster/leave is documented on NO platform's capability
    table (#448 — the firmware serves it but omits it from its own /api
    index), so the client-side gate must not block it on S3."""
    posts = []
    def handler(req):
        if req.method == "POST":
            posts.append(req.url.path)
            return httpx.Response(200, text="left")
        return httpx.Response(200, json={})
    cfg = Config(boards=[Board("leader", "http://x")], default="leader")
    app = SplitflapApp(cfg, client_factory=lambda url: BoardClient(
        url, transport=httpx.MockTransport(handler)))
    async with app.run_test() as pilot:
        assert app.plat == "esp32s3"       # default before any poll
        await pilot.press(":")
        await pilot.press(*"cluster leave")
        await pilot.press("enter")
        await pilot.pause(0.2)
        await pilot.press(*"cluster-leave")   # typed-confirm token
        await pilot.press("enter")
        await pilot.pause(0.3)
    assert "/cluster/leave" in posts


@pytest.mark.asyncio
async def test_ctrl_s_stops_immediately_without_confirm():
    """Item 2: ctrl+s is a priority binding — fires even though nothing is
    focused on the command bar, and runs stop with no confirm modal."""
    posts = []
    def handler(req):
        if req.method == "POST":
            posts.append(req.url.path)
            return httpx.Response(200, json={"seq": 1})
        return httpx.Response(200, json={})
    cfg = Config(boards=[Board("leader", "http://x")], default="leader")
    app = SplitflapApp(cfg, client_factory=lambda url: BoardClient(
        url, transport=httpx.MockTransport(handler)))
    async with app.run_test() as pilot:
        await pilot.press("ctrl+s")
        await pilot.pause(0.3)
    assert "/stop" in posts


@pytest.mark.asyncio
async def test_ctrl_s_fires_even_while_command_input_focused():
    """Item 2: "always active" — must still fire while the command Input
    has focus (priority=True bypasses the normal focus-first dispatch)."""
    posts = []
    def handler(req):
        if req.method == "POST":
            posts.append(req.url.path)
            return httpx.Response(200, json={"seq": 1})
        return httpx.Response(200, json={})
    cfg = Config(boards=[Board("leader", "http://x")], default="leader")
    app = SplitflapApp(cfg, client_factory=lambda url: BoardClient(
        url, transport=httpx.MockTransport(handler)))
    async with app.run_test() as pilot:
        await pilot.press(":")              # opens + focuses #command
        await pilot.press("ctrl+s")
        await pilot.pause(0.3)
    assert "/stop" in posts


@pytest.mark.asyncio
async def test_command_history_recalls_last_entry():
    """Item 3: k9s-style history — up/down cycle previously submitted
    lines while the command Input is focused."""
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
        await pilot.press(*"text FIRST")
        await pilot.press("enter")
        await pilot.pause(0.2)
        await pilot.press(":")
        await pilot.press(*"text SECOND")
        await pilot.press("enter")
        await pilot.pause(0.2)
        await pilot.press(":")
        await pilot.press("up")
        await pilot.pause(0.1)
        cmd = app.query_one("#command", Input)
        assert cmd.value == "text SECOND"
        await pilot.press("up")
        await pilot.pause(0.1)
        assert cmd.value == "text FIRST"
        await pilot.press("down")
        await pilot.pause(0.1)
        assert cmd.value == "text SECOND"


@pytest.mark.asyncio
async def test_cluster_config_sends_members_form_param():
    """Item 4: outbound request shape for `cluster config <table>` — the
    verified wire contract is a POST FORM param named "members", NOT a
    query param (WebCluster.cpp:483-491)."""
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
        await pilot.press(*"cluster config host1|0|0|16;host2|1|0|5")
        await pilot.press("enter")
        await pilot.pause(0.2)
        await pilot.press(*"config")          # typed-confirm token
        await pilot.press("enter")
        await pilot.pause(0.3)
    assert any(path == "/cluster/config" and "members=" in body
              and "host1" in body for path, body in posts)
    # query string must NOT carry it — that would be the wrong transport
    assert not any("?members=" in path for path, _ in posts)


# ---- #441 finding 3: self-test's own result contract, not /unit/op-result

@pytest.mark.asyncio
async def test_self_test_polls_its_own_result_endpoint_and_renders_measurements():
    """/unit/op-result's "ok" doesn't carry measurements, and on the
    follower means only "started" — false success is possible there. This
    pins the fix: submit to /unit/self-test, then poll
    /unit/self-test-result (never /unit/op-result), and render the
    measurements in the status line."""
    calls = []
    def handler(req):
        calls.append(req.url.path)
        if req.url.path == "/unit/self-test" and req.method == "POST":
            return httpx.Response(200, json={"seq": 9})
        if req.url.path == "/unit/self-test-result":
            return httpx.Response(200, json={"state": "ok", "steps_per_rev": 2038,
                                             "hall_window": 46, "rev_time_ms": 1200})
        return httpx.Response(200, json={})
    cfg = Config(boards=[Board("leader", "http://x")], default="leader")
    app = SplitflapApp(cfg, client_factory=lambda url: BoardClient(
        url, transport=httpx.MockTransport(handler)))
    async with app.run_test() as pilot:
        await pilot.press(":")
        await pilot.press(*"op self-test 3")
        await pilot.press("enter")
        await pilot.pause(0.2)
        await pilot.press("y")              # plain confirm tier (TIER_CONFIRM)
        await pilot.pause(0.3)
        status = app.query_one("#cmd-status", Static)
    assert "/unit/self-test-result" in calls
    assert "/unit/op-result" not in calls
    assert "steps_per_rev=2038" in status.content
    assert "hall_window=46" in status.content
    assert "rev_time_ms=1200" in status.content


@pytest.mark.asyncio
async def test_self_test_failure_renders_reason_and_unit_reason():
    def handler(req):
        if req.url.path == "/unit/self-test" and req.method == "POST":
            return httpx.Response(200, json={"seq": 3})
        if req.url.path == "/unit/self-test-result":
            return httpx.Response(200, json={"state": "failed",
                                             "reason": "unit-failed",
                                             "unit_reason": "hall-stuck",
                                             "steps_per_rev": 0, "hall_window": 0,
                                             "rev_time_ms": 0})
        return httpx.Response(200, json={})
    cfg = Config(boards=[Board("leader", "http://x")], default="leader")
    app = SplitflapApp(cfg, client_factory=lambda url: BoardClient(
        url, transport=httpx.MockTransport(handler)))
    async with app.run_test() as pilot:
        await pilot.press(":")
        await pilot.press(*"op self-test 6")
        await pilot.press("enter")
        await pilot.pause(0.2)
        await pilot.press("y")
        await pilot.pause(0.3)
        status = app.query_one("#cmd-status", Static)
    assert "hall-stuck" in status.content
    assert "unit-failed" in status.content


# ---- #441 finding 4: `reboot [board]` targets a named board, not always
# the default one.

@pytest.mark.asyncio
async def test_reboot_with_board_arg_targets_that_boards_url():
    posts: list[tuple[str, str]] = []
    def factory(url: str) -> BoardClient:
        name = "leader" if url == "http://leader" else "row0"
        def handler(req):
            if req.method == "POST":
                posts.append((name, req.url.path))
                return httpx.Response(200, text="rebooting")
            return httpx.Response(200, json={})
        return BoardClient(url, transport=httpx.MockTransport(handler))
    cfg = Config(boards=[Board("leader", "http://leader"),
                         Board("row0", "http://row0")], default="leader")
    app = SplitflapApp(cfg, client_factory=factory)
    async with app.run_test() as pilot:
        await pilot.press(":")
        await pilot.press(*"reboot row0")
        await pilot.press("enter")
        await pilot.pause(0.2)
        await pilot.press(*"row0")            # typed-confirm token = board name
        await pilot.press("enter")
        await pilot.pause(0.3)
    assert ("row0", "/reboot") in posts
    assert ("leader", "/reboot") not in posts


@pytest.mark.asyncio
async def test_reboot_unknown_board_shows_error_and_sends_no_request():
    posts = []
    def handler(req):
        if req.method == "POST":
            posts.append(req.url.path)
        return httpx.Response(200, json={})
    cfg = Config(boards=[Board("leader", "http://leader")], default="leader")
    app = SplitflapApp(cfg, client_factory=lambda url: BoardClient(
        url, transport=httpx.MockTransport(handler)))
    async with app.run_test() as pilot:
        await pilot.press(":")
        await pilot.press(*"reboot bogus")
        await pilot.press("enter")
        await pilot.pause(0.2)
        await pilot.press(*"bogus")           # typed-confirm token = board name
        await pilot.press("enter")
        await pilot.pause(0.3)
        status = app.query_one("#cmd-status", Static)
    assert "/reboot" not in posts
    assert "unknown board" in status.content
    assert "bogus" in status.content


@pytest.mark.asyncio
async def test_reboot_without_board_arg_still_targets_default():
    posts = []
    def handler(req):
        if req.method == "POST":
            posts.append(req.url.path)
            return httpx.Response(200, text="rebooting")
        return httpx.Response(200, json={})
    cfg = Config(boards=[Board("leader", "http://leader")], default="leader")
    app = SplitflapApp(cfg, client_factory=lambda url: BoardClient(
        url, transport=httpx.MockTransport(handler)))
    async with app.run_test() as pilot:
        await pilot.press(":")
        await pilot.press(*"reboot")
        await pilot.press("enter")
        await pilot.pause(0.2)
        await pilot.press(*"reboot")          # typed-confirm token falls back
        await pilot.press("enter")            # to the command name
        await pilot.pause(0.3)
    assert "/reboot" in posts


def test_help_examples_cover_every_command_exactly():
    names = {parse(e.example).name for e in HELP}
    assert names == set(CANONICAL_NAMES)


def test_help_entries_are_complete():
    for e in HELP:
        assert e.usage and e.tier and e.blurb and e.example
