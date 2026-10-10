"""A home job ends on what the unit says of its search (#605).

The unit acknowledges SFP_CMD_HOME before anything turns, so a job graded by
that acknowledgement reads done over a search that failed. Both boards wait
for the search and grade it by shared/HomeWait.h; the wait and the grades are
tested natively (test_home_wait), the board glue is pinned here by source.
"""
from pathlib import Path

V2 = Path(__file__).resolve().parents[1] / "firmware/v2"
MASTER = (V2 / "Master/DisplayTask.cpp").read_text()
ROW_JOBS = (V2 / "FollowerEsp01/FollowerUnitJobs.cpp").read_text()
ROW_BUS = (V2 / "FollowerEsp01/FollowerBus.cpp").read_text()


def body(source: str, signature: str) -> str:
    start = source.index(signature)
    return source[start:source.index("\n}\n", start)]


def test_master_home_is_graded_by_the_wait():
    home = body(MASTER, "static void execHome(")
    assert "grade = waitForHome(cmd.unitAddress)" in home
    assert "displayApplyMaintResult(local, cmd, grade)" in home


def test_master_home_shows_the_frame_after_the_search():
    home = body(MASTER, "static void execHome(")
    assert home.index("waitForHome(") < home.index("reshowLastFrame(local)")


def test_master_wait_asks_the_shared_rule_and_can_be_stopped():
    wait = body(MASTER, "static MaintGrade waitForHome(")
    assert "homeWaitObserve(" in wait and "maintGradeHome(outcome)" in wait
    assert "unitBusAbortRequested()" in wait and "wdtFeed()" in wait


def test_master_home_all_is_graded_by_the_units():
    assert "gradeHomeAll(local)" in body(MASTER, "static void execResetUnits(")
    grade = body(MASTER, "static MaintGrade gradeHomeAll(")
    assert "homeAllTallyAdd(" in grade and "maintGradeHomeAll(tally)" in grade


def test_row_home_leaves_its_result_to_the_poll():
    case = ROW_JOBS[ROW_JOBS.index("case FollowerOpKind::Home:"):]
    case = case[:case.index("case FollowerOpKind::Identify:")]
    assert "homeWaitBegin(homePoll.wait" in case
    # Nothing is stamped while the search runs.
    assert case.index("stagedOp.pending = false;") < case.index("return;")


def test_row_poll_stamps_the_shared_grade():
    poll = body(ROW_JOBS, "static void pollHome(")
    assert "homeWaitObserve(" in poll
    assert "stampOpResult(homePoll.seq, maintGradeHome(outcome))" in poll
    assert "pollHome();" in body(ROW_JOBS, "void unitJobsLoopTick(")


def test_row_holds_its_one_job_slot_while_a_home_is_waited_on():
    assert "homePoll.seq != 0" in body(ROW_JOBS, "static bool opSlotBusy(")


def test_row_home_all_is_graded_by_the_units():
    case = ROW_JOBS[ROW_JOBS.index("case FollowerOpKind::HomeAll:"):]
    assert "grade = busGradeHomeAll();" in case[:case.index("break;")]
    grade = body(ROW_BUS, "MaintGrade busGradeHomeAll(")
    assert "homeAllTallyAdd(" in grade and "maintGradeHomeAll(tally)" in grade
