"""Source gate for the call order of the row's long bus sequences.

busRunReflashJob, followerBootHome and busShowSegment are Wire glue no native
test reaches, and their order is the behaviour: the quiet wait before any unit
leaves for twiboot, the settle and the health poll before the boot-home, the
inrush stagger before each write, the verify pass after the row stops. Pin the
landmarks in order.

File-agnostic: the bus glue is read from every FollowerBus*.cpp, so a function
may sit in any of them. A step split out of a pinned function is listed in
STEPS and read in place of its call, so the order holds across the split.
"""
import re
from pathlib import Path

TREE = Path(__file__).resolve().parents[1]

# Static helpers a pinned function calls for one of its steps: their bodies
# are read where the call stands.
STEPS = ("reflashWaitRowQuiet", "reflashPlanJobTargets")

PINNED = {
    "void busRunReflashJob(uint8_t onlyAddr, bool force)": (
        "reflashProgressBegin(reflashProgress, 0);",
        "unitUpdateCollectSketchUnits(",
        "waitForBatchIdle(rowUnits, rowCount, UNIT_UPDATE_QUIET_MS)",
        "reflashCollectForcedTarget(",
        "reflashCollectRebootTargets(",
        "reflashPlanTargets(",
        "n = reflashFilterToAddress(flashTargets, n, onlyAddr);",
        "reflashProgress.total = (uint8_t)n;",
        "flashBootloaderUnits(flashTargets, n, handResetWaitMs);",
        "reflashProgressSettling(reflashProgress);",
        "jobPollHealth();",
        "followerBootHome();",
        "bool bootHalted = !halted && runBootSweep(onlyAddr);",
        "jobPollHealth();",
        "reflashProgressFinish(reflashProgress, false, halted || bootHalted);",
    ),
    "void followerBootHome()": (
        "bootHomeCollectTargets(",
        "if (n == 0) return;",
        "busHome(targets[j]);",
        "waitForBatchIdle(batch, batchN, BOOT_HOME_BATCH_TIMEOUT_MS);",
        "delay(BOOT_HOME_SETTLE_MS);",
        "busPollHealth();",
    ),
    "void busShowSegment(const String& segment, int webSpeed)": (
        "lastFrameValid = true;",
        "waitForRowToStop();",
        "admitMotion();",
        "waitForMotionSlot(movers);",
        "renderStaggerShouldSettle(commandedCount, RENDER_STAGGER_BATCH)",
        "delay(RENDER_STAGGER_SETTLE_MS);",
        "writeToUnit(i, letter, speed);",
        "motionTrackerAdd(movers, i, millis());",
        "waitForRowToStop();",
        "unitReadDisplayedLetter(",
        "waitForMotionSlot(resendMovers);",
        "writeToUnit(i, commanded[i], speed);",
        "motionTrackerAdd(resendMovers, i, millis());",
        "if (resent > 0) waitForRowToStop();",
        "settleReadPending = true;",
        "settleReadTick();",
    ),
}


def _strip_comments(text):
    """Drop // and /* */ comments, honouring string and char literals."""
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if c in "\"'":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            out.append(text[i:j + 1])
            i = j + 1
        elif text.startswith("//", i):
            j = text.find("\n", i)
            i = n if j < 0 else j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            i = n if j < 0 else j + 2
        else:
            out.append(c)
            i += 1
    return "".join(out)


def _bus_code():
    files = sorted(TREE.glob("FollowerBus*.cpp"))
    assert files, "no FollowerBus*.cpp in the tree"
    return "\n".join(_strip_comments(p.read_text()) for p in files)


def _body_at(code, open_brace):
    depth = 0
    for k in range(open_brace, len(code)):
        if code[k] == "{":
            depth += 1
        elif code[k] == "}":
            depth -= 1
            if depth == 0:
                return code[open_brace:k + 1]
    raise AssertionError("unbalanced braces")


def _definition(code, signature):
    """The body of the one definition whose text starts with `signature`."""
    hits = [m.start() for m in re.finditer(re.escape(signature), code)
            if code.find("{", m.start()) < code.find(";", m.start())]
    assert len(hits) == 1, f"{signature!r}: {len(hits)} definitions"
    return _body_at(code, code.index("{", hits[0]))


def _step_body(code, name):
    m = re.search(r"\bstatic\s[^;{}()]*\b" + name + r"\s*\([^;{}]*\)\s*\{", code)
    return _body_at(code, m.end() - 1) if m else None


def _flattened(code, signature):
    body = _definition(code, signature)
    for name in STEPS:
        step = _step_body(code, name)
        if step is None:
            continue
        calls = list(re.finditer(r"\b" + name + r"\s*\([^;]*;", body))
        assert len(calls) == 1, f"{name} called {len(calls)} times in {signature!r}"
        c = calls[0]
        body = body[:c.start()] + step + body[c.end():]
    return body


def _assert_in_order(body, landmarks, signature):
    at = 0
    for mark in landmarks:
        found = body.find(mark, at)
        assert found >= 0, f"{signature!r}: {mark!r} missing or out of order"
        at = found + len(mark)


def test_the_pinned_sequences_keep_their_order():
    code = _bus_code()
    for signature, landmarks in PINNED.items():
        _assert_in_order(_flattened(code, signature), landmarks, signature)


def test_a_halted_flash_starts_no_boot_sweep():
    # The short-circuit is the rule: a halted flash run does not start a
    # second kind of write.
    body = _flattened(_bus_code(), "void busRunReflashJob(uint8_t onlyAddr, bool force)")
    assert body.count("runBootSweep(") == 1
    assert "!halted && runBootSweep(onlyAddr)" in body


def test_the_gate_reads_every_bus_file():
    names = {p.name for p in TREE.glob("FollowerBus*.cpp")}
    assert "FollowerBus.cpp" in names
