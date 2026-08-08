from splitflap_client.models import ClusterMember, ClusterStatus
from splitflap_tui.widgets import DOT_BAD, DOT_OK, DOT_WARN, member_dot_style


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
