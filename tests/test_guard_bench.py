"""Pins .claude/hooks/guard-bench.sh (#651): what an agent may not run, and
what asks in the main checkout. A guard nobody breaks on purpose is unproven."""
import json
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
GUARD = ROOT / ".claude" / "hooks" / "guard-bench.sh"

pytestmark = pytest.mark.skipif(shutil.which("jq") is None, reason="the guard needs jq")


def decision(command, *, agent, cwd=ROOT):
    payload = json.dumps({"tool_input": {"command": command}, "cwd": str(cwd)})
    args = ["bash", str(GUARD)] + (["--agent"] if agent else [])
    out = subprocess.run(args, input=payload, capture_output=True, text=True,
                         env={"PATH": "/usr/bin:/bin", "CLAUDE_PROJECT_DIR": str(ROOT)},
                         check=True).stdout.strip()
    if not out:
        return "allow"
    return json.loads(out)["hookSpecificOutput"]["permissionDecision"]


@pytest.mark.parametrize("command", [
    "flashing/ota-flash.sh -p esp32 board",
    "./flashing/commission-units.sh",
    "flashing/restore-unit-offsets.sh replay",
    "python3 flashing/ext-diag-benchcheck.py",
    "python3 flashing/release.py --channel test",
    "flashing/make-release-key.sh",
    "pio run -t upload",
    "cd firmware/v2/Unit && pio run --target upload -e unit",
    "esptool.py --port /dev/ttyACM0 write_flash 0x0 a.bin",
    "esptool write-flash 0x830000 rescue.bin",
    "esptool erase-flash",
    "curl -X POST http://board/api/v2/x",
    "curl --request DELETE http://board/x",
    "curl -d a=1 http://board/x",
    "curl --data-binary @f http://board/x",
    "curl -F file=@a.bin http://board/firmware",
])
def test_agent_bench_commands_are_denied(command):
    assert decision(command, agent=True) == "deny"


@pytest.mark.parametrize("command", [
    "pio run",
    "pio test -e native",
    "scripts/check.sh --touched master",
    "curl -s https://example.com/doc",
    "esptool image-info a.bin",
    "git -C .claude/worktrees/wt-1 commit -m x",
])
def test_agent_build_commands_pass(command):
    assert decision(command, agent=True) == "allow"


def test_main_session_keeps_the_bench():
    assert decision("flashing/ota-flash.sh -p esp32 board", agent=False) == "allow"


@pytest.mark.parametrize("command", [
    "git checkout -b foo",
    "git switch other",
    "git reset --hard HEAD~1",
    "git status && git reset --soft HEAD~1",
])
def test_branch_moves_in_the_main_checkout_ask(command):
    assert decision(command, agent=False) == "ask"
    assert decision(command, agent=True) == "ask"


def test_branch_moves_in_a_slot_pass():
    assert decision("git -C /x/.claude/worktrees/wt-1 reset --hard", agent=True) == "allow"
    assert decision("git status", agent=False) == "allow"
