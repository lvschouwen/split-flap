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
