#!/usr/bin/env python3
"""Code standard gate (#653). Limits and rationale: docs/CODING_STANDARDS.md 1.5.

Run by scripts/check.sh (every tree selection). By hand:
`python3 -I scripts/check_standard.py [--report | --shrink | --rename OLD NEW | --init]`.

Static metrics (lizard: NLOC per file; length, CCN, nesting and parameters per function) are ratcheted: a value past
the fail line fails unless tools/standard-baseline.txt lists it at that value or more. The baseline may only shrink,
and the gate enforces that against the merge-base version of the file. Warn-line hits print for the files changed
since the merge base. Ported from the why2025-badge gate (#202); its stack and image-size parts are not used here.

Nothing passes silently: a tool that is missing, fails, or measures less than was asked for fails the gate. The one
bypass is SPLITFLAP_SKIP_STANDARD=1, which prints loudly."""
import os
import re
import subprocess
import sys
import tempfile

# metric: (warn, fail). A value above warn warns, above fail fails.
LIMITS = {
    "nloc": (800, 1500),
    "len": (60, 150),
    "ccn": (15, 25),
    "nest": (4, 6),
    "params": (6, 8),
}
MOVED = "moved"  # 'moved<TAB>metric:old=>new<TAB>0': a deliberate rename, see apply_rename
BASELINE_METRICS = set(LIMITS) | {MOVED}
CPP_EXT = (".c", ".cpp", ".h", ".hpp", ".ino")
SOURCE_EXT = CPP_EXT + (".py",)
CPP_ROOT = "firmware/v2/"
PY_ROOTS = ("flashing/", "firmware/v2/buildtools/")
SKIP_PREFIXES = ("firmware/v2/UnitBootloader/",)
SKIP_PARTS = ("/test/", "/tests/", "/.pio/", "/managed_components/", "/node_modules/")
BASELINE_REL = "tools/standard-baseline.txt"
DEFAULT_HEADER = "# Code standard baseline (#653), see docs/CODING_STANDARDS.md 1.5. metric<TAB>key<TAB>value."


class GateError(Exception):
    pass


# --- baseline ---------------------------------------------------------------------------------------------------

def parse_baseline(text):
    """Lines 'metric<TAB>key<TAB>value'; '#' lines and blanks are comments. Returns {(metric, key): int}."""
    out = {}
    for n, line in enumerate(text.splitlines(), 1):
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        parts = line.split("\t")
        if len(parts) != 3 or not parts[2].strip().isdigit():
            raise GateError("baseline line %d is not 'metric<TAB>key<TAB>value': %r" % (n, line))
        if parts[0] not in BASELINE_METRICS:
            raise GateError("baseline line %d: unknown metric %r (known: %s)" % (n, parts[0], ", ".join(sorted(BASELINE_METRICS))))
        entry = (parts[0], parts[1])
        if entry in out:
            raise GateError("baseline line %d: %s %s is listed twice" % (n, parts[0], parts[1]))
        out[entry] = int(parts[2])
    return out


def format_baseline(header, entries):
    lines = [header.rstrip("\n")]
    for (metric, key), value in sorted(entries.items()):
        lines.append("%s\t%s\t%d" % (metric, key, value))
    return "\n".join(lines) + "\n"


def split_header(text):
    """The leading comment block of a baseline file, kept when it is rewritten."""
    head = []
    for line in text.splitlines():
        if line.startswith("#") or not line.strip():
            head.append(line)
        else:
            break
    return "\n".join(head)


def atomic_write(path, text):
    """Temp file in the same directory, then rename: an interrupted run never leaves a half-written baseline."""
    fd, tmp = tempfile.mkstemp(dir=os.path.dirname(path), prefix=".baseline-")
    try:
        with os.fdopen(fd, "w") as f:
            f.write(text)
        os.replace(tmp, path)
    except BaseException:
        if os.path.exists(tmp):
            os.unlink(tmp)
        raise


# --- measuring ------------------------------------------------------------------------------------------------

def is_ours(path):
    if any(p in "/" + path for p in SKIP_PARTS) or path.startswith(SKIP_PREFIXES):
        return False
    if path.startswith(CPP_ROOT) and path.endswith(CPP_EXT):
        return True
    return path.startswith(PY_ROOTS) and path.endswith(".py")


_FUNC_RE = re.compile(r"^\s*(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(.+)@(\d+)-(\d+)@(.+)$")
_FILE_RE = re.compile(r"^\s*(\d+)\s+[\d.]+\s+[\d.]+\s+[\d.]+\s+(\d+)\s+(\S.*\.(?:c|cpp|h|hpp|ino|py))\s*$")

def parse_lizard(text):
    """Parse lizard -ENS text output into {(metric, key): value}. Functions are keyed 'path:name'; further functions
    of the same name in one file (C++ overloads, static helpers under #ifdef) get '#2', '#3' in source order. The
    ordinal is stable across edits elsewhere in the file and needs no line numbers."""
    out = {}
    seen = {}
    listed = set()
    for line in text.splitlines():
        m = _FUNC_RE.match(line)
        if m:
            _nloc, ccn, _tok, params, length, nest, name, first, last, path = m.groups()
            if (path, name, first, last) in listed:
                continue  # lizard prints the over-threshold functions a second time in its warnings table
            listed.add((path, name, first, last))
            base = "%s:%s" % (path, name.strip())
            seen[base] = seen.get(base, 0) + 1
            key = base if seen[base] == 1 else "%s#%d" % (base, seen[base])
            out[("len", key)] = int(length)
            out[("ccn", key)] = int(ccn)
            out[("nest", key)] = int(nest)
            out[("params", key)] = int(params)
            continue
        m = _FILE_RE.match(line)
        if m and "@" not in line:
            out[("nloc", m.group(3).strip())] = int(m.group(1))
    return out


def _git(root, *args):
    return subprocess.run(["git", "-C", root] + list(args), check=True, capture_output=True, text=True).stdout


def tracked_sources(root):
    """Tracked and untracked-but-not-ignored files: a new file counts before it is `git add`ed."""
    names = set(_git(root, "ls-files", "-z").split("\0"))
    names.update(_git(root, "ls-files", "-z", "--others", "--exclude-standard").split("\0"))
    return sorted(f for f in names if is_ours(f) and os.path.isfile(os.path.join(root, f)))


def lizard_cmd():
    """The lizard of the interpreter running the gate (the pinned check venv), else the one on PATH."""
    probe = subprocess.run([sys.executable, "-m", "lizard", "--version"], capture_output=True, text=True)
    if probe.returncode == 0:
        return [sys.executable, "-m", "lizard"]
    return ["lizard"]


def measure_lizard(root, files, lizard=None):
    """Returns (measured, problems). A chunk where lizard fails, or a file without an nloc row, is a problem: the
    gate must not pass because less was measured than asked. -i lifts lizard's own exit-1-on-warnings."""
    out, problems = {}, []
    lizard = lizard or lizard_cmd()
    for i in range(0, len(files), 200):
        chunk = files[i : i + 200]
        try:
            res = subprocess.run(list(lizard) + ["-ENS", "-i", "1000000"] + chunk, cwd=root, capture_output=True, text=True)
        except OSError as e:
            return out, ["cannot run lizard: %s" % e]
        if res.returncode != 0:
            problems.append("lizard failed (exit %d) on %d files starting at %s: %s"
                            % (res.returncode, len(chunk), chunk[0], res.stderr.strip()[:300]))
            continue
        got = parse_lizard(res.stdout)
        out.update(got)
        problems.extend("lizard returned no result for %s" % f for f in chunk if ("nloc", f) not in got)
    return out, problems


# --- comparing ------------------------------------------------------------------------------------------------

def compare_static(measured, baseline, changed_files=None):
    """The ratchet. Returns (failures, warnings, stale) as lists of strings.

    failures: a value past the fail line that is not baselined, or is above its baselined value.
    warnings: a value past the warn line (not past fail) in a changed file (all files if changed_files is None).
    stale: baseline entries that are fixed, lowered or gone; the caller fails on them (run --shrink)."""
    failures, warnings, stale = [], [], []
    for (metric, key), value in sorted(measured.items()):
        if metric not in LIMITS:
            continue
        warn, fail = LIMITS[metric]
        base = baseline.get((metric, key))
        if value > fail:
            if base is None:
                failures.append("NEW   %s %s = %d (fail line %d)" % (metric, key, value, fail))
            elif value > base:
                failures.append("WORSE %s %s = %d (baseline %d, fail line %d)" % (metric, key, value, base, fail))
        elif value > warn:
            path = key.split(":")[0]
            if changed_files is None or path in changed_files:
                warnings.append("warn  %s %s = %d (warn line %d)" % (metric, key, value, warn))
    for (metric, key), base in sorted(baseline.items()):
        if metric not in LIMITS:
            continue
        cur = measured.get((metric, key))
        if cur is None:
            stale.append("%s %s: gone from the tree" % (metric, key))
        elif cur < base:
            stale.append("%s %s: %d -> %d%s" % (metric, key, base, cur, "" if cur > LIMITS[metric][1] else " (below the fail line)"))
    return failures, warnings, stale


def check_shrink_only(baseline, base_baseline):
    """The 'may only shrink' rule against the merge-base version of the file. A static entry that is new or higher
    fails, unless a 'moved' record in the baseline says it is the old entry under a new name (and the old entry was at
    least as high). Returns failure strings."""
    out = []
    moved = {}
    for (metric, key) in baseline:
        if metric == MOVED and "=>" in key:
            head, new = key.split("=>", 1)
            m, old = head.split(":", 1) if ":" in head else ("", head)
            # a record counts only when the old entry is really gone from the current baseline
            if (m, old) not in baseline:
                moved.setdefault((m, new), []).append(old)
    claimed = set()
    for (metric, key), value in sorted(baseline.items()):
        if metric not in LIMITS:
            continue
        base = base_baseline.get((metric, key))
        if base is None:
            olds = [o for o in moved.get((metric, key), []) if (metric, o) not in claimed]
            old = olds[0] if len(olds) == 1 else None  # one old key moves to one new key
            old_val = base_baseline.get((metric, old)) if old is not None else None
            if old_val is not None:
                claimed.add((metric, old))
            if old_val is None or value > old_val:
                out.append("GREW  baseline %s %s = %d is not in the merge-base baseline (new entries are not allowed; "
                           "use --rename for a moved offender)" % (metric, key, value))
        elif value > base:
            out.append("GREW  baseline %s %s raised %d -> %d (a baseline only shrinks)" % (metric, key, base, value))
    return out

def shrink(measured, baseline, problems=()):
    """The baseline after a ratchet step: entries fixed or gone are dropped, entries that got smaller are lowered.
    It never adds an entry or raises one. Refuses when the measurement was incomplete (it would drop real offenders)."""
    if problems:
        raise GateError("refusing to shrink, the measurement is incomplete: " + "; ".join(problems))
    out = {}
    for (metric, key), base in baseline.items():
        if metric not in LIMITS:
            out[(metric, key)] = base
            continue
        cur = measured.get((metric, key))
        if cur is not None and cur > LIMITS[metric][1]:
            out[(metric, key)] = min(base, cur)
    return out


def apply_rename(baseline, old, new):
    """Move the static entries of OLD to NEW keeping their values. OLD is a file (renames its nloc entry and every
    'file:function' one) or a 'file:function' key. Each move leaves a 'moved' record that check_shrink_only accepts
    against the merge-base baseline. Returns the new baseline and the list of moves."""
    out, moves = dict(baseline), []
    for (metric, key), value in baseline.items():
        if metric not in LIMITS:
            continue
        if key == old or key.startswith(old + ":") or key.startswith(old + "#"):
            new_key = new + key[len(old):]
            del out[(metric, key)]
            out[(metric, new_key)] = value
            out[(MOVED, "%s:%s=>%s" % (metric, key, new_key))] = 0
            moves.append("%s %s -> %s = %d" % (metric, key, new_key, value))
    if not moves:
        raise GateError("no baseline entry for %s" % old)
    return out, moves


def drop_settled_moves(baseline, base_baseline):
    """'moved' records are only needed until the merge base has the new key."""
    out = dict(baseline)
    for (metric, key) in baseline:
        if metric == MOVED and "=>" in key:
            head, new = key.split("=>", 1)
            if (head.split(":", 1)[0], new) in base_baseline:
                del out[(metric, key)]
    return out


def initial_baseline(measured):
    return {k: v for k, v in measured.items() if k[0] in LIMITS and v > LIMITS[k[0]][1]}


# --- main -----------------------------------------------------------------------------------------------------

def merge_base(root):
    for ref in ("origin/master", "master"):
        r = subprocess.run(["git", "-C", root, "merge-base", "HEAD", ref], capture_output=True, text=True)
        if r.returncode == 0 and r.stdout.strip():
            return r.stdout.strip()
    return ""


def changed_since_base(root):
    base = merge_base(root)
    names = set(_git(root, "ls-files", "-z", "--others", "--exclude-standard").split("\0"))
    if base:
        names.update(_git(root, "diff", "-z", "--name-only", "--diff-filter=d", base).split("\0"))
    return names


def base_baseline_text(root):
    """The baseline at the merge base (HEAD when there is none). None when the file does not exist there yet: the
    change that introduces the baseline has nothing to be compared with."""
    for ref in (merge_base(root) or "HEAD", "HEAD"):
        r = subprocess.run(["git", "-C", root, "show", "%s:%s" % (ref, BASELINE_REL)], capture_output=True, text=True)
        if r.returncode == 0:
            return r.stdout
    return None


def unparsed_files(measured):
    """Warn lines for big C/C++ files with no function row: lizard probably misparsed them."""
    with_funcs = {key.rsplit(":", 1)[0] for (metric, key) in measured if metric == "len" and ":" in key}
    return ["WARN  %s has %d NLOC and no function rows (lizard may have misparsed it)" % (key, v)
            for (metric, key), v in sorted(measured.items())
            if metric == "nloc" and v > 200 and key.endswith((".c", ".cpp", ".ino")) and key not in with_funcs]


def run_check(baseline, base_baseline, measured, problems, changed):
    """The gate proper. Returns (failures, warnings, notes)."""
    fails = ["INCOMPLETE " + p for p in problems]
    notes = []
    f, warns, stale = compare_static(measured, baseline, changed)
    warns += unparsed_files(measured)
    fails += f
    fails += ["STALE baseline %s (run: python3 -I scripts/check_standard.py --shrink)" % s for s in stale]
    if base_baseline is not None:
        fails += check_shrink_only(baseline, base_baseline)
    else:
        notes.append("no baseline at the merge base: this change introduces it, shrink-only check skipped")
    return fails, warns, notes


def repo_root():
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main(argv):
    if os.environ.get("SPLITFLAP_SKIP_STANDARD") == "1":
        print("standard: !!! SKIPPED by SPLITFLAP_SKIP_STANDARD=1: the code standard is NOT checked !!!", file=sys.stderr)
        return 0
    root = repo_root()
    mode = argv[1] if len(argv) > 1 else ""
    base_path = os.path.join(root, *BASELINE_REL.split("/"))
    exists = os.path.exists(base_path)
    text = ""
    if exists:
        with open(base_path) as f:
            text = f.read()
    baseline = parse_baseline(text)

    if mode == "--init" and exists:
        raise GateError("%s exists; --init only creates the first baseline" % BASELINE_REL)
    if mode == "--rename":
        if len(argv) != 4:
            raise GateError("usage: check_standard.py --rename OLD NEW (file path, or file:function)")
        new, moves = apply_rename(baseline, argv[2], argv[3])
        atomic_write(base_path, format_baseline(split_header(text), new))
        print("\n".join(moves))
        return 0

    measured, problems = measure_lizard(root, tracked_sources(root))
    if mode == "--init":
        if problems:
            raise GateError("refusing to create a baseline from an incomplete measurement: " + "; ".join(problems))
        os.makedirs(os.path.dirname(base_path), exist_ok=True)
        atomic_write(base_path, format_baseline(DEFAULT_HEADER, initial_baseline(measured)))
        return 0
    if mode == "--shrink":
        new = shrink(measured, baseline, problems)
        base_text = base_baseline_text(root)
        if base_text is not None:
            new = drop_settled_moves(new, parse_baseline(base_text))
        atomic_write(base_path, format_baseline(split_header(text), new))
        return 0
    if mode == "--report":
        for p in problems:
            print("note: " + p, file=sys.stderr)
        for (metric, key), value in sorted(measured.items(), key=lambda kv: (kv[0][0], -kv[1])):
            if value > LIMITS[metric][0]:
                print("%-6s %6d  %s%s" % (metric, value, key, "  FAIL" if value > LIMITS[metric][1] else ""))
        return 0
    if mode:
        raise GateError("unknown option %s" % mode)

    if not merge_base(root):
        print("standard: !!! no origin/master or master ref resolves: the GREW (shrink-only) check compares against "
              "HEAD's baseline only !!!", file=sys.stderr)
    base_text = base_baseline_text(root)
    fails, warns, notes = run_check(baseline, parse_baseline(base_text) if base_text is not None else None,
                                    measured, problems, changed_since_base(root))
    for line in notes:
        print("standard: " + line)
    for line in warns:
        print(line)
    for line in fails:
        print(line, file=sys.stderr)
    if fails:
        print("standard: %d problem(s). Fix the code; a baseline only shrinks, see docs/CODING_STANDARDS.md 1.5" % len(fails),
              file=sys.stderr)
        return 1
    print("standard: ok (%d metrics measured, %d warnings in changed files)" % (len(measured), len(warns)))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv))
    except GateError as e:
        print("standard: %s" % e, file=sys.stderr)
        sys.exit(1)
