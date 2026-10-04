"""Source gate for the boot-reflash brake (#513).

The brake is two `if`s in main.cpp glue that no native test can reach. Pin
them: every boot-time unit flash must sit behind prefsReflashOnBoot(), and the
preference must be loaded before the first of them.
"""
import re
from pathlib import Path

TREE = Path(__file__).resolve().parents[1]
MAIN = TREE / "main.cpp"
BOOT_FLASHERS = ("busAutoInstallBootloaderUnits()", "busAutoUpdateOutdatedUnits()")


def _strip_comments(text):
    """Drop // and /* */ comments, honouring string and char literals — a
    naive regex treats the "/*" in a "// .../cluster/* ..." comment as a block
    opener and swallows the code after it."""
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


def _code(path=None):
    return _strip_comments((path or MAIN).read_text())


def test_comment_stripper_keeps_code_after_a_glob_in_a_line_comment():
    src = '// the /cluster/* handlers\nint a = 1; /* gone */ const char* u = "http://x/*y";\n'
    assert _strip_comments(src) == '\nint a = 1;  const char* u = "http://x/*y";\n'


def _enclosing_conditions(code, pos):
    """Conditions of every `if (...) {` block open at `pos`, plus a same-line
    `if (...) stmt;` guard."""
    conds, stack = [], []
    for m in re.finditer(r"if\s*\(([^{};]*)\)\s*\{|\{|\}", code[:pos]):
        tok = m.group(0)
        if tok == "}":
            if stack:
                stack.pop()
        else:
            stack.append(m.group(1))  # None for a bare `{`
    conds = [c for c in stack if c]
    line_start = code.rfind("\n", 0, pos) + 1
    inline = re.match(r"\s*if\s*\((.*)\)\s*$", code[line_start:pos])
    if inline:
        conds.append(inline.group(1))
    return conds


def test_every_boot_time_unit_flash_is_behind_the_brake():
    code = _code()
    sites = [m.start() for name in BOOT_FLASHERS for m in re.finditer(re.escape(name), code)]
    assert len(sites) >= 3, "expected the early install plus the settled install+update"
    for pos in sites:
        conds = _enclosing_conditions(code, pos)
        assert any("prefsReflashOnBoot()" in c and "!" not in c for c in conds), (
            f"unbraked boot flash near: {code[max(0, pos - 80):pos + 40]!r}")


def test_preference_is_loaded_before_the_first_boot_flash():
    code = _code()
    init = code.index("prefsInit();")
    assert code.index("clusterInit();") < init, "prefsInit needs clusterInit's EEPROM.begin"
    assert init < min(code.index(name) for name in BOOT_FLASHERS)


def _function_body(code, signature):
    i = code.index("{", code.index(signature))
    depth = 0
    for j in range(i, len(code)):
        depth += {"{": 1, "}": -1}.get(code[j], 0)
        if depth == 0:
            return code[i + 1:j]
    raise AssertionError(f"unterminated body for {signature}")


def test_a_staged_setting_is_persisted_before_a_restart():
    code = _code()
    reboot = code[code.index("if (isPendingReboot)"):]
    assert reboot.index("prefsLoopTick(true);") < reboot.index("ESP.restart();")


def test_targeted_run_flashes_only_the_planned_list():
    # The flash loop (shared reflashRunTargets) walks the list it is handed,
    # so narrowing that list is what keeps a ?address= run off the other
    # bootloader-mode units.
    loop = _function_body(_code(TREE / "FollowerBus.cpp"),
                          "static bool flashBootloaderUnits(const uint8_t* targets")
    assert "reflashRunTargets(hooks, targets, count, reflashProgress)" in loop
    assert "unitFacts[" not in loop, "the loop must not re-derive targets from the facts"
    job = _function_body(_code(TREE / "FollowerBus.cpp"),
                         "void busRunReflashJob(uint8_t onlyAddr)")
    assert job.count("reflashFilterToAddress(") == 2  # reboot sweep + planned total
    narrowed = job.index("n = reflashFilterToAddress(flashTargets, n, onlyAddr);")
    assert narrowed < job.index("flashBootloaderUnits(flashTargets, n);")


def test_reflash_route_never_falls_through_to_the_bulk_job_on_a_bad_address():
    code = _code(TREE / "FollowerWeb.cpp")
    route = code[code.index('server.on("/reflash-units"'):]
    route = route[:route.index("reflashPending = true;")]
    body_refusal = route.index('request->hasParam("address", true)')
    query = route.index('request->hasParam("address")')
    assert body_refusal < query
    assert "reflashParseAddress(" in route and "reflashAddressInRange(" in route
    assert "queryRequireLong" not in route, "strtol base 0 must not pick the unit"
    # Both address branches end in a return before the bulk path.
    assert route[body_refusal:query].count("return;") == 1
    assert route[query:].count("return;") >= 2
