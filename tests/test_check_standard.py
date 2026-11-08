"""Host test of the code standard gate (#653): parsing, ratchet, shrink-only, tooling failures, scope."""
import os
import subprocess
import sys
import tempfile

import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import check_standard as cs  # noqa: E402

LIZARD = """\
NLOC    CCN   token  PARAM  length  NS  location
------------------------------------------------
      9      1     42      0       9     0 small@10-18@firmware/v2/Master/a.cpp
    120     30    900      9     160     7 big@20-180@firmware/v2/Master/a.cpp
     12      2     80      1      12     1 big@200-212@firmware/v2/Master/a.cpp
==============================================================
NLOC    Avg.NLOC  AvgCCN  Avg.token  function_cnt    file
--------------------------------------------------------------
   1600      14.7     3.4       82.9        26     firmware/v2/Master/a.cpp
    300      14.7     3.4       82.9        26     flashing/x.py
"""
A = "firmware/v2/Master/a.cpp"


def test_parse_lizard_reads_every_metric_and_dedupes_repeat_names():
    m = cs.parse_lizard(LIZARD)
    assert m[("ccn", A + ":big")] == 30
    assert m[("len", A + ":big")] == 160
    assert m[("nest", A + ":big")] == 7
    assert m[("params", A + ":big")] == 9
    assert m[("nloc", A)] == 1600
    assert m[("nloc", "flashing/x.py")] == 300
    assert m[("ccn", A + ":big#2")] == 2


def test_baseline_roundtrip_and_errors():
    b = cs.parse_baseline("# c\nccn\ta.c:f\t30\n\nnloc\ta.c\t900\n")
    assert b == {("ccn", "a.c:f"): 30, ("nloc", "a.c"): 900}
    assert cs.parse_baseline(cs.format_baseline("# c", b)) == b
    for bad in ("ccn a.c:f 30\n", "ccn\ta.c:f\t30\nccn\ta.c:f\t20\n", "bogus\ta.c:f\t30\n", "stack\ta.c:f\t30\n"):
        with pytest.raises(cs.GateError):
            cs.parse_baseline(bad)


def test_scope():
    assert cs.is_ours("firmware/v2/Master/DisplayTask.cpp")
    assert cs.is_ours("firmware/v2/Unit/Unit.ino")
    assert cs.is_ours("firmware/v2/shared/X.h")
    assert cs.is_ours("flashing/flasher/make_manifest.py")
    assert cs.is_ours("firmware/v2/buildtools/fwbuild.py")
    assert not cs.is_ours("firmware/v2/UnitBootloader/twiboot/x.c")
    assert not cs.is_ours("firmware/v2/Master/test/test_x/test_x.cpp")
    assert not cs.is_ours("flashing/tests/test_x.py")
    assert not cs.is_ours("firmware/v2/Master/.pio/libdeps/x.cpp")
    assert not cs.is_ours("firmware/v2/Master/managed_components/x/y.c")
    assert not cs.is_ours("firmware/v1/ESPMaster/x.cpp")
    assert not cs.is_ours("scripts/check_standard.py")


def test_new_offender_fails():
    f, _, _ = cs.compare_static({("ccn", "a.c:f"): 26}, {})
    assert f and f[0].startswith("NEW")


def test_baselined_passes_but_worse_fails():
    base = {("ccn", "a.c:f"): 30}
    assert cs.compare_static({("ccn", "a.c:f"): 30}, base)[0] == []
    f, _, _ = cs.compare_static({("ccn", "a.c:f"): 31}, base)
    assert f and f[0].startswith("WORSE")


def test_at_the_fail_line_is_not_a_violation():
    for metric, (_warn, fail) in cs.LIMITS.items():
        assert cs.compare_static({(metric, "a.c:f"): fail}, {})[0] == []
        assert cs.compare_static({(metric, "a.c:f"): fail + 1}, {})[0]


def test_warn_only_in_changed_files_and_never_fails():
    m = {("len", "a.c:f"): 70, ("len", "b.c:g"): 70}
    f, w, _ = cs.compare_static(m, {}, {"a.c"})
    assert f == [] and len(w) == 1 and "a.c:f" in w[0]
    assert len(cs.compare_static(m, {}, None)[1]) == 2


def test_stale_when_fixed_lowered_or_gone():
    base = {("ccn", "a.c:f"): 30, ("ccn", "a.c:g"): 30, ("ccn", "a.c:h"): 30}
    _, _, stale = cs.compare_static({("ccn", "a.c:f"): 20, ("ccn", "a.c:g"): 28}, base)
    assert len(stale) == 3
    assert any("gone" in s for s in stale)


def test_shrink_never_adds_or_raises():
    base = {("ccn", "a.c:f"): 30, ("ccn", "a.c:g"): 30, ("ccn", "a.c:h"): 30}
    m = {("ccn", "a.c:f"): 20, ("ccn", "a.c:g"): 28, ("ccn", "a.c:h"): 40, ("ccn", "a.c:new"): 50}
    assert cs.shrink(m, base) == {("ccn", "a.c:g"): 28, ("ccn", "a.c:h"): 30}
    with pytest.raises(cs.GateError):
        cs.shrink(m, base, ["lizard failed"])


def test_grew_against_the_merge_base():
    mb = {("ccn", "a.c:f"): 30}
    assert cs.check_shrink_only({("ccn", "a.c:f"): 30}, mb) == []
    assert cs.check_shrink_only({("ccn", "a.c:f"): 29}, mb) == []
    assert cs.check_shrink_only({("ccn", "a.c:f"): 31}, mb)[0].startswith("GREW")
    assert cs.check_shrink_only({("ccn", "a.c:f"): 30, ("ccn", "a.c:g"): 26}, mb)[0].startswith("GREW")


def test_rename_keeps_value_and_passes_the_shrink_check():
    mb = {("nloc", "a.c"): 900, ("ccn", "a.c:f"): 30, ("ccn", "other.c:f"): 30}
    new, moves = cs.apply_rename(mb, "a.c", "b.c")
    assert new[("nloc", "b.c")] == 900 and new[("ccn", "b.c:f")] == 30 and ("ccn", "a.c:f") not in new
    assert len(moves) == 2 and ("ccn", "other.c:f") in new
    assert cs.check_shrink_only(new, mb) == []
    raised, _ = cs.apply_rename({("ccn", "a.c:f"): 30}, "a.c:f", "a.c:g")
    raised[("ccn", "a.c:g")] = 31
    assert cs.check_shrink_only(raised, {("ccn", "a.c:f"): 30})
    with pytest.raises(cs.GateError):
        cs.apply_rename(mb, "missing.c", "x.c")


def test_settled_move_records_are_dropped():
    new, _ = cs.apply_rename({("ccn", "a.c:f"): 30}, "a.c", "b.c")
    assert any(k[0] == cs.MOVED for k in new)
    settled = cs.drop_settled_moves(new, {("ccn", "b.c:f"): 30})
    assert not any(k[0] == cs.MOVED for k in settled)


def test_run_check_fails_on_stale_incomplete_and_grew():
    f, _, _ = cs.run_check({("ccn", "a.c:f"): 30}, {("ccn", "a.c:f"): 30}, {("ccn", "a.c:f"): 20}, ["lizard failed"], None)
    assert any(x.startswith("STALE") for x in f) and any(x.startswith("INCOMPLETE") for x in f)
    f, _, _ = cs.run_check({("ccn", "a.c:f"): 30}, {}, {("ccn", "a.c:f"): 30}, [], None)
    assert any(x.startswith("GREW") for x in f)
    f, _, notes = cs.run_check({("ccn", "a.c:f"): 30}, None, {("ccn", "a.c:f"): 30}, [], None)
    assert f == [] and notes


def test_initial_baseline_holds_only_offenders():
    m = {("ccn", "a.c:f"): 30, ("ccn", "a.c:g"): 25, ("nloc", "a.c"): 10}
    assert cs.initial_baseline(m) == {("ccn", "a.c:f"): 30}


def test_missing_lizard_is_a_problem_not_a_skip():
    _, problems = cs.measure_lizard(ROOT, ["scripts/check_standard.py"], lizard=["/nonexistent/lizard"])
    assert problems and "cannot run lizard" in problems[0]


def test_lizard_failure_and_missing_rows_are_problems():
    _, problems = cs.measure_lizard(ROOT, ["firmware/v2/x.cpp"], lizard=[sys.executable, "-c", "import sys; sys.exit(3)"])
    assert problems and "exit 3" in problems[0]
    _, problems = cs.measure_lizard(ROOT, ["firmware/v2/a.cpp"], lizard=[sys.executable, "-c", "print('')"])
    assert any("no result" in p for p in problems)


def test_real_lizard_measures_cpp_ino_and_py(tmp_path):
    (tmp_path / "a.cpp").write_text("int f(int a){ return a; }\n")
    (tmp_path / "b.ino").write_text("void loop(){ if (1) { } }\n")
    (tmp_path / "c.py").write_text("def g(x):\n    return x\n")
    m, problems = cs.measure_lizard(str(tmp_path), ["a.cpp", "b.ino", "c.py"])
    assert problems == []
    assert {("nloc", "a.cpp"), ("nloc", "b.ino"), ("nloc", "c.py"), ("ccn", "b.ino:loop"), ("ccn", "c.py:g")} <= set(m)


def test_untracked_files_are_measured(tmp_path):
    subprocess.run(["git", "-C", str(tmp_path), "init", "-q"], check=True, capture_output=True)
    d = tmp_path / "firmware" / "v2" / "Master"
    d.mkdir(parents=True)
    (d / "new.cpp").write_text("int f(){return 1;}\n")
    (tmp_path / ".gitignore").write_text("ignored.cpp\n")
    (d / "ignored.cpp").write_text("int g(){return 1;}\n")
    assert cs.tracked_sources(str(tmp_path)) == ["firmware/v2/Master/new.cpp"]


def test_skip_env_is_loud_and_the_only_bypass(monkeypatch, capsys):
    monkeypatch.setenv("SPLITFLAP_SKIP_STANDARD", "1")
    assert cs.main(["x"]) == 0
    assert "SKIPPED" in capsys.readouterr().err


def test_atomic_write_replaces_and_leaves_no_temp():
    with tempfile.TemporaryDirectory() as d:
        p = os.path.join(d, "b.txt")
        cs.atomic_write(p, "one\n")
        cs.atomic_write(p, "two\n")
        assert open(p).read() == "two\n" and os.listdir(d) == ["b.txt"]


def test_committed_baseline_parses():
    with open(os.path.join(ROOT, *cs.BASELINE_REL.split("/"))) as f:
        assert cs.parse_baseline(f.read())


# --- review follow-up -------------------------------------------------------------------------------------------

def _git(repo, *a):
    return subprocess.run(["git", "-C", str(repo)] + list(a), check=True, capture_output=True, text=True).stdout


def _repo(tmp_path):
    _git(tmp_path, "init", "-q", "-b", "master")
    _git(tmp_path, "config", "user.email", "t@example.invalid")
    _git(tmp_path, "config", "user.name", "t")
    (tmp_path / "README").write_text("x\n")
    _git(tmp_path, "add", "-A")
    _git(tmp_path, "commit", "-q", "-m", "init")
    return tmp_path


def test_gate_step_runs_for_every_tree_selection():
    lines = open(os.path.join(ROOT, "scripts", "check.sh")).read().splitlines()
    gate = [i for i, ln in enumerate(lines) if "check_standard.py" in ln and ln.startswith('step "code standard gate"')]
    assert len(gate) == 1, "the gate step must be a top-level (unindented) step, outside run_py"
    loop = next(i for i, ln in enumerate(lines) if ln.startswith('for t in "${ALL_TREES[@]}"; do'))
    venv = next(i for i, ln in enumerate(lines) if ln == "ensure_venv")
    assert venv < gate[0] < loop


def test_moved_record_with_old_entry_still_present_is_laundering():
    mb = {("ccn", "a.c:f"): 30}
    cur = {("ccn", "a.c:f"): 30, ("ccn", "b.c:g"): 30, (cs.MOVED, "ccn:a.c:f=>b.c:g"): 0}
    assert cs.check_shrink_only(cur, mb)[0].startswith("GREW")


def test_one_old_key_moves_to_only_one_new_key():
    mb = {("ccn", "a.c:f"): 30}
    cur = {("ccn", "b.c:g"): 30, ("ccn", "c.c:h"): 30,
           (cs.MOVED, "ccn:a.c:f=>b.c:g"): 0, (cs.MOVED, "ccn:a.c:f=>c.c:h"): 0}
    got = cs.check_shrink_only(cur, mb)
    assert len(got) == 1 and got[0].startswith("GREW")


def test_baseline_introduced_in_head_is_compared_against_head(tmp_path):
    repo = _repo(tmp_path)
    (repo / "tools").mkdir()
    (repo / cs.BASELINE_REL).write_text("ccn\ta.c:f\t30\n")
    _git(repo, "add", "-A")
    _git(repo, "commit", "-q", "-m", "baseline")
    _git(repo, "update-ref", "refs/remotes/origin/master", "HEAD~1")
    assert "a.c:f" in cs.base_baseline_text(str(repo))  # merge base lacks the file: HEAD's copy is used


def test_no_master_ref_is_loud_and_other_rules_still_run(tmp_path, monkeypatch, capsys):
    repo = _repo(tmp_path)
    _git(repo, "branch", "-m", "side")
    d = repo / "firmware" / "v2" / "Master"
    d.mkdir(parents=True)
    body = "int f(int a){\n" + "".join("if(a==%d){a++;}\n" % i for i in range(30)) + "return a;}\n"
    (d / "big.cpp").write_text(body)
    monkeypatch.setattr(cs, "repo_root", lambda: str(repo))
    assert cs.merge_base(str(repo)) == ""
    assert cs.main(["x"]) == 1  # NEW offender: ccn past 25 and no baseline entry
    err = capsys.readouterr().err
    assert "no origin/master or master ref" in err and "NEW" in err


def test_non_ascii_paths_are_listed(tmp_path):
    repo = _repo(tmp_path)
    d = repo / "firmware" / "v2" / "Master"
    d.mkdir(parents=True)
    (d / "café.cpp").write_text("int f(){return 1;}\n")
    assert cs.tracked_sources(str(repo)) == ["firmware/v2/Master/café.cpp"]
    _git(repo, "add", "-A")
    assert cs.tracked_sources(str(repo)) == ["firmware/v2/Master/café.cpp"]


def test_big_file_without_function_rows_warns_not_fails():
    m = {("nloc", "firmware/v2/Master/x.cpp"): 500, ("nloc", "firmware/v2/Master/y.cpp"): 500,
         ("len", "firmware/v2/Master/y.cpp:f"): 5, ("nloc", "firmware/v2/Master/z.h"): 500}
    f, w, _ = cs.run_check({}, None, m, [], set())
    assert f == []
    assert len(w) == 1 and "x.cpp" in w[0]
