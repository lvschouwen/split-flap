from splitflap_client.models import ClusterMember, UnitsHealth
from splitflap_tui.widgets import (DOT_BAD, DOT_OK, DOT_WARN, UnitsTable,
                                   member_dot_style, units_rows)


def member(**over):
    d = {"host": "192.168.15.121", "row": 0, "col": 0, "width": 5,
         "joined": True, "failures": 0, "rev": "9f694dd", "plat": "esp01"}
    d.update(over)
    return ClusterMember.from_json(d)


def test_dot_ok_when_joined_and_clean():
    assert member_dot_style(member()) == DOT_OK


def test_dot_warn_on_suspect_or_update_blocked():
    assert member_dot_style(member(suspect=True)) == DOT_WARN
    assert member_dot_style(member(updateBlocked=True)) == DOT_WARN


def test_dot_bad_on_lost_degraded_rescue_or_stuck():
    assert member_dot_style(member(joined=False)) == DOT_BAD
    assert member_dot_style(member(degraded=True)) == DOT_BAD
    assert member_dot_style(member(rescue=True)) == DOT_BAD
    assert member_dot_style(member(renderStuck=True)) == DOT_BAD


from splitflap_client.models import SystemStatsNow
from splitflap_tui.widgets import StatsBar, format_cmd_status


def test_format_cmd_status_success_line():
    t = format_cmd_status("ok — set text: 'HI'", 0.42, "22:03:41")
    assert t.plain == "✓ 22:03:41  ok — set text: 'HI' (0.4 s)"


def test_format_cmd_status_error_strips_prefix_and_keeps_body_literal():
    t = format_cmd_status("⛔ 409: [cluster] busy", 1.26, "09:00:00")
    assert t.plain == "⛔ 09:00:00  409: [cluster] busy (1.3 s)"


def test_format_cmd_status_without_duration():
    t = format_cmd_status("cancelled", None, "12:00:00")
    assert t.plain == "✓ 12:00:00  cancelled"


def test_stats_bar_humanizes():
    s = SystemStatsNow.from_json({"heap": 182432, "minHeap": 141000,
                                  "rssi": -54, "uptime": 271234,
                                  "i2cTx": 12043, "i2cErr": 0})
    bar = StatsBar()
    lines = []
    bar.update = lambda text: lines.append(text)      # capture, no app needed
    bar.update_stats(s, True)
    assert lines[-1] == ("connected · heap 182k (min 141k) · rssi -54 · "
                         "up 3d 3h · i2c 12043/0 err")


from splitflap_tui.widgets import SUGGEST_WORDS, CommandInput


async def test_command_input_suggests_command_names():
    inp = CommandInput()
    assert inp.suggester is not None
    assert await inp.suggester.get_suggestion("clu") == "cluster leave"
    assert await inp.suggester.get_suggestion("st") == "stop"
    assert "help" in SUGGEST_WORDS


def test_suggest_words_cover_every_command():
    from splitflap_tui.app import DISPLAY_NAMES
    from splitflap_tui.commands import CANONICAL_NAMES
    expected = {DISPLAY_NAMES.get(n, n) for n in CANONICAL_NAMES} | {"help"}
    assert set(SUGGEST_WORDS) == expected


# ---- #455: lifetime wear in the units table ---------------------------
def _wall_row():
    """The REAL row 1 as read off the live leader on 2026-08-13: sixteen
    units, of which a6 carries hf=3 with sxl 972 and a15 sits at sxl 1465
    against a median of 18."""
    sxl = [17, 18, 37, 18, 17, 972, 17, 18, 18, 18, 17, 17, 17, 18, 1465, 17]
    units = []
    for i, v in enumerate(sxl):
        u = {"i": i, "a": i + 1, "st": 1, "v": 1, "sx": 0, "odo": 20,
             "vmin": 5000, "sxl": v}
        if i + 1 == 6:
            u["hf"] = 3
        units.append(u)
    return UnitsHealth.from_json({"width": 16, "faulty": 0, "units": units})


def test_units_table_shows_lifetime_not_since_boot():
    table = UnitsTable()
    assert "sxl" in table.COLUMNS and "hf" in table.COLUMNS
    assert "sx" not in table.COLUMNS, "sx read 0 on every live unit"


def test_units_table_flags_the_two_degrading_units():
    rows = units_rows(_wall_row())
    flags = {r[0]: r[-1] for r in rows}
    assert flags["0x01"] == "" and flags["0x05"] == ""
    assert "HALL" in flags["0x06"] and "DRAG" in flags["0x06"]
    assert flags["0x0f"] == "DRAG"


def test_units_table_renders_lifetime_values():
    rows = {r[0]: r for r in units_rows(_wall_row())}
    assert rows["0x0f"][2] == "1465"      # sxl column
    assert rows["0x06"][3] == "3"         # hf column
    assert rows["0x01"][3] == "—"         # absent hf stays an em dash


# ---- #471: Tab accepts the ghost completion --------------------------
import httpx
from textual.widgets import Static
import pytest
from splitflap_client.transport import BoardClient
from splitflap_tui.app import SplitflapApp
from splitflap_tui.config import Board, Config
from splitflap_tui.screens.discover_screen import DiscoverScreen


def _app(posts=None):
    def handler(req):
        if req.method == "POST":
            if posts is not None:
                posts.append(req.url.path)
            if req.url.path == "/cluster/discover":
                return httpx.Response(200, text="Board discovery started")
            return httpx.Response(200, text="ok")
        if req.url.path == "/cluster/discover":
            return httpx.Response(200, json={"status": "done", "boards": []})
        return httpx.Response(200, json={})
    cfg = Config(boards=[Board("leader", "http://x")], default="leader")
    return SplitflapApp(cfg, client_factory=lambda url: BoardClient(
        url, transport=httpx.MockTransport(handler)))


async def test_tab_accepts_the_ghost_completion():
    app = _app()
    async with app.run_test() as pilot:
        await pilot.press(":")
        await pilot.press(*"disc")
        await pilot.pause(0.1)
        cmd = app.query_one("#command", CommandInput)
        assert cmd._suggestion == "discover"    # ghost is showing
        assert cmd.value == "disc"              # but not yet accepted
        await pilot.press("tab")
        await pilot.pause(0.1)
        assert cmd.value == "discover"
        assert cmd.cursor_position == len("discover")


async def test_tab_keeps_focus_on_the_bar_when_there_is_no_suggestion():
    """Tab must never fall through to focus-next: a bar that loses focus
    mid-typing is the #454 failure class."""
    app = _app()
    async with app.run_test() as pilot:
        await pilot.press(":")
        await pilot.press(*"zzz")
        await pilot.pause(0.1)
        cmd = app.query_one("#command", CommandInput)
        await pilot.press("tab")
        await pilot.pause(0.1)
        assert cmd.value == "zzz"
        assert app.focused is cmd


async def test_unique_prefix_plus_enter_never_expands_to_a_kill_command():
    """The deliberate non-feature (#471): `stop` is TIER_KILL and runs with
    NO confirm, so auto-accepting a unique prefix on Enter would let `st` +
    Enter blank the wall from a typo. Enter stays literal."""
    posts = []
    app = _app(posts)
    async with app.run_test() as pilot:
        await pilot.press(":")
        await pilot.press(*"st")
        await pilot.press("enter")
        await pilot.pause(0.3)
        status = app.query_one("#cmd-status", Static)
        assert "unknown command: st" in status.content
    assert "/stop" not in posts


async def test_tab_then_enter_runs_the_completed_command():
    app = _app()
    async with app.run_test() as pilot:
        await pilot.press(":")
        await pilot.press(*"disc")
        await pilot.press("tab")
        await pilot.press("enter")
        await pilot.pause(0.4)
        assert isinstance(app.screen, DiscoverScreen)
