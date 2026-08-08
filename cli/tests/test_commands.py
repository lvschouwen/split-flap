import pytest
from splitflap_tui.commands import (CommandError, TIER_CONFIRM, TIER_KILL,
                                    TIER_ROUTINE, TIER_TYPED, parse)


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


def test_unknown_command_raises_usage():
    with pytest.raises(CommandError):
        parse("frobnicate")


import httpx
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
