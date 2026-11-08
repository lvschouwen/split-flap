"""Pins .claude/hooks/guard-bench.sh (#651): what an agent may not run, and
what asks in the main checkout. A guard nobody breaks on purpose is unproven."""
import json
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
GUARD = ROOT / ".claude" / "hooks" / "guard-bench.sh"


def test_jq_is_installed():
    # The guard fails closed for agents without jq; a skipped suite would hide that.
    assert shutil.which("jq"), "guard-bench.sh needs jq"


def run_guard(payload, *, agent, path="/usr/bin:/bin"):
    args = ["bash", str(GUARD)] + (["--agent"] if agent else [])
    out = subprocess.run(args, input=payload, capture_output=True, text=True,
                         env={"PATH": path, "CLAUDE_PROJECT_DIR": str(ROOT)},
                         check=True).stdout.strip()
    if not out:
        return "allow"
    return json.loads(out)["hookSpecificOutput"]["permissionDecision"]


def decision(command, *, agent, cwd=ROOT):
    payload = json.dumps({"tool_input": {"command": command}, "cwd": str(cwd)})
    return run_guard(payload, agent=agent)


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
    "curl -sd a=1 http://board/x",
    "curl -sXPOST http://board/x",
    "curl --json '{}' http://board/x",
    "platformio run -t upload",
    "pio run --target=upload",
    "wget --post-data=a http://board/x",
    "http POST board/x a=1",
    "python3 -c 'import requests; requests.post(\"http://board\")'",
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
    "curl -sSLf https://example.com/doc",
    "curl -o out.html https://example.com",
])
def test_agent_build_commands_pass(command):
    assert decision(command, agent=True) == "allow"


def test_main_session_keeps_the_bench():
    assert decision("flashing/ota-flash.sh -p esp32 board", agent=False) == "allow"


def test_without_agent_flag_nothing_is_checked():
    # Branch moves in the main checkout belong to the global guard-main-checkout.sh.
    assert decision("git reset --hard", agent=False) == "allow"


def test_agent_fails_closed_without_jq(tmp_path):
    (tmp_path / "bash").symlink_to(shutil.which("bash"))
    for tool in ("cat", "git", "grep", "sed", "tail", "printf"):
        if shutil.which(tool):
            (tmp_path / tool).symlink_to(shutil.which(tool))
    payload = json.dumps({"tool_input": {"command": "pio run"}, "cwd": str(ROOT)})
    assert run_guard(payload, agent=True, path=str(tmp_path)) == "deny"
    assert run_guard(payload, agent=False, path=str(tmp_path)) == "allow"


def test_agent_fails_closed_on_unparsable_input():
    assert run_guard("not json", agent=True) == "deny"
    assert run_guard("not json", agent=False) == "allow"
