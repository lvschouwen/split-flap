"""No logic in two firmware trees (epic #538).

Each test pins one piece of logic to its single home under
`firmware/v2/shared/` by forbidding the vocabulary a private re-implementation
would need. `tests/test_twiboot_shared.py` is the same idea for the twiboot
client.
"""

import re
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
V2 = REPO / "firmware/v2"
SHARED = V2 / "shared"
ROW_MASTERS = [V2 / "Master", V2 / "FollowerEsp01"]
TREES = ROW_MASTERS + [V2 / "Rescue"]
SKIP_DIRS = {".pio", "test", "tests", "managed_components", ".dummy"}
SOURCE_SUFFIXES = {".cpp", ".cc", ".h", ".hpp", ".ino", ".ini"}


def _strip_comments(src: str) -> str:
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.DOTALL)
    return re.sub(r"//[^\n]*", "", src)


def _sources(trees):
    for tree in trees:
        for path in sorted(tree.rglob("*")):
            if path.suffix not in SOURCE_SUFFIXES:
                continue
            if SKIP_DIRS & set(path.relative_to(tree).parts):
                continue
            yield path


def _offenders(pattern, trees=TREES):
    found = {}
    for path in _sources(trees):
        text = path.read_text()
        if path.suffix != ".ini":
            text = _strip_comments(text)
        hits = re.findall(pattern, text)
        if hits:
            found[str(path.relative_to(REPO))] = sorted(set(hits))
    return found


def test_the_gate_reads_the_row_master_sources():
    seen = set(_sources(TREES))
    assert V2 / "Master/DisplayTask.cpp" in seen
    assert V2 / "FollowerEsp01/FollowerBus.cpp" in seen
    assert V2 / "Master/platformio.ini" in seen


# --- #528: flap letter + speed mapping --------------------------------------

def test_no_tree_maps_characters_to_flaps_itself():
    """A private walk over the alphabet is how the ESP-01 row came to skip
    lowercase and unknown characters."""
    assert not _offenders(r"\bSFP_ALPHABET\b"), (
        "characters map to flap indices only in shared/FlapLetters.h")


def test_the_wire_speed_range_is_defined_once():
    assert not _offenders(r"(?:#define\s+|-D\s*)M(?:IN|AX)_SPEED\b"), (
        "MIN_SPEED/MAX_SPEED live in shared/FlapLetters.h")
    assert not _offenders(r"\bmap\s*\(\s*\w+\s*,\s*1\s*,\s*100\b"), (
        "the web speed conversion is convertSpeedToUnit()")


def test_the_follower_render_uses_the_shared_mapping():
    body = _strip_comments(
        (V2 / "FollowerEsp01/FollowerBus.cpp").read_text())
    assert "flapLetterOrBlank(" in body
    assert "convertSpeedToUnit(" in body


# --- #527: maintenance vocabulary + reflash plan ------------------------------

def test_no_tree_redefines_the_maintenance_vocabulary():
    """FollowerOps.h was a hand copy of three Master headers and missed the
    #405 protocol-mismatch clause and the #412 consecutive-failure halt."""
    owned = (r"\b(?:enum class (?:MaintOutcome|MaintReason|SelfTestOutcome|"
             r"ReflashState|OpResultState)|struct (?:MaintVerdict|SelfTestSlot|"
             r"ReflashProgress)|inline \w[\w\s\*]* (?:maintValidate\w+|"
             r"maintEncode\w+|maint\w+Name|reflash[A-Z]\w+|buildOpResultJson|"
             r"buildSelfTestJson|buildReflashJson|selfTestOutcomeName)\()")
    assert not _offenders(owned), (
        "op validators, result vocabulary and the reflash plan live in "
        "shared/MaintenancePolicy.h and shared/ReflashPlan.h")


def test_the_reflash_batch_size_is_a_build_flag_per_row_master():
    for tree, size in (("Master", "4"), ("FollowerEsp01", "2")):
        ini = (V2 / tree / "platformio.ini").read_text()
        flags = re.findall(r"-D\s*REFLASH_BATCH_SIZE=(\d+)", ini)
        assert flags == [size, size], f"{tree}: target and native env"
    assert not _offenders(r"#define\s+REFLASH_BATCH_SIZE\b")


def test_both_reflash_jobs_run_the_shared_loop():
    """The loop carries the batch throttle and the consecutive-failure halt."""
    loop = _strip_comments((SHARED / "ReflashPlan.h").read_text())
    assert "reflashShouldHalt(consecutiveFailures)" in loop
    for path in (V2 / "Master/DisplayTask.cpp",
                 V2 / "FollowerEsp01/FollowerBus.cpp"):
        body = _strip_comments(path.read_text())
        assert "reflashRunTargets(" in body, path.name
        assert "reflashShouldHalt(" not in body, path.name


def test_a_unit_enters_its_bootloader_for_its_own_flash_only():
    """The bootloader goes back to the firmware when it is left alone, so a
    row sent in up front loses the units whose turn comes late (#577)."""
    loop = _strip_comments((SHARED / "ReflashPlan.h").read_text())
    run = loop[loop.index("inline ReflashRunEnd reflashRunTargets("):]
    assert run.index("reflashEnterUnit(h, addr)") < run.index("h.flashUnit(addr)")
    for path in (V2 / "Master/DisplayTask.cpp",
                 V2 / "FollowerEsp01/FollowerBus.cpp"):
        body = _strip_comments(path.read_text())
        assert "reflashPlanTargets(" in body, path.name
        # The one order a flash job sends is the loop's hook.
        swept = re.findall(r"for\s*\([^)]*\)[^;{]*\{?[^;]*RebootToBootloader\(",
                           body)
        assert not swept, f"{path.name}: units sent in ahead of their flash"


# --- #529/#530: self-test wait, op grading, fact patches ---------------------

def test_no_tree_grades_an_op_outcome_by_hand():
    """The same op used to report wire-fail on one platform and
    postcondition-fail on the other."""
    hits = _offenders(r"\?\s*MaintOutcome::Ok\s*:\s*MaintOutcome::\w+",
                      ROW_MASTERS)
    assert not hits, f"grade through maintGrade*(): {hits}"


def test_no_tree_judges_self_test_replies_itself():
    assert not _offenders(r"\bSELFTEST_STATE_\w+|\bsawRunning\b|"
                          r"#define\s+SELF_TEST_\w+|"
                          r"\bconstexpr\s+\w+\s+SELF_TEST_\w+", ROW_MASTERS), (
        "the self-test wait is shared/SelfTestPoll.h")
    for path in ("Master/DisplayTask.cpp", "FollowerEsp01/FollowerUnitJobs.cpp"):
        assert "selfTestPollObserve(" in (V2 / path).read_text(), path


def test_unit_read_validity_is_patched_through_the_shared_helpers():
    hits = _offenders(r"\b(?:offsetValid|odometerValid)\s*=(?!=)", ROW_MASTERS)
    # The bus files set them where they READ the value, and the master where
    # it reads a row board's unit facts off the link; nothing else may.
    readers = {"firmware/v2/Master/UnitBus.cpp",
               "firmware/v2/FollowerEsp01/FollowerBus.cpp",
               "firmware/v2/Master/UnitFactsJson.h"}
    assert set(hits) <= readers, hits
    for path in ("Master/DisplayIpc.h", "FollowerEsp01/FollowerBus.cpp"):
        assert "unitFactsInvalidateReads(" in (V2 / path).read_text(), path


# --- #531: LAN origin + CSRF ---------------------------------------------------

def test_the_lan_origin_rule_exists_once():
    """A security boundary that was three byte-identical copies (plus a
    fourth private-IPv4 parser for the SSRF guard)."""
    assert not _offenders(r"\boctets\[|startsWith\(\"http://\"\)|"
                          r"inline bool \w+(?:PrivateIpv4|OriginAllowed|"
                          r"CsrfReject\w*|SameOrigin|IsLanTarget)\b"), (
        "LAN host/origin checks and the CSRF reject live in shared/LanOrigin.h")
    users = {"Master/WebEndpoints.cpp",
             "FollowerEsp01/FollowerWeb.cpp", "Rescue/RescueWeb.cpp"}
    for path in users:
        assert "lanCsrfReject(" in (V2 / path).read_text(), path


def test_the_follower_reprobes_a_unit_whose_reads_it_invalidated():
    """Only a probe re-reads the offset on the ESP-01 row, and nothing probes
    periodically there: an op that invalidates a unit's reads must queue one."""
    web = _strip_comments((V2 / "FollowerEsp01/FollowerUnitJobs.cpp").read_text())
    body = web[web.index("static void executeStagedOp()"):
               web.index("static void pollSelfTest()")]
    for kind in ("RebootToBootloader", "BootUpdate", "BootDump"):
        case = body[body.index(f"case FollowerOpKind::{kind}:"):]
        case = case[:case.index("case FollowerOpKind::", 10)
                    if "case FollowerOpKind::" in case[10:] else len(case)]
        assert "unitHealthRefreshPending = true;" in case, kind


# --- #536: member phase machine, fault mask, unit timings ---------------------

def test_the_member_phase_machine_exists_once():
    assert not _offenders(r"enum class (?:Cluster)?FollowerPhase\b|"
                          r"struct (?:ClusterFollower|FollowerCluster)State\b|"
                          r"inline \w[\w\s]* \w*[Ff]ollower\w*"
                          r"(?:Tick|AcceptRender|JoinConflicts)\(|"
                          r"\w+_CONTACT_FRESH_MS\s*=|\b(?:CLUSTER|FOLLOWER)_GRACE_MS\s*=|"
                          r"inline size_t \w*FaultMaskHex\("), (
        "the phase machine is shared/ClusterMemberPhase.h, the fault mask "
        "shared/UnitHealth.h")


def test_unit_timings_are_named_once():
    """The probe inhibit was a named constant on the S3 and a bare 3000 at
    seven sites on the ESP-01."""
    hits = _offenders(r"ProbeInhibit\(millis\(\)\s*\+\s*\d+|\bdelay\(\s*1500\s*\)|"
                      r"#define\s+(?:TWIBOOT_STARTUP_MS|SHOW_STUCK_TIMEOUT_MS)|"
                      r"constexpr\s+uint32_t\s+(?:ADDRESS_OP_SETTLE_MS|"
                      r"SHOW_STUCK_TIMEOUT_MS)", ROW_MASTERS)
    assert not hits, f"use shared/UnitTimings.h: {hits}"


# --- #532: OTA upload gate ----------------------------------------------------

UPLOAD_PATHS = ["Master/WebFirmware.cpp", "Rescue/RescueWeb.cpp",
                "FollowerEsp01/FollowerWeb.cpp"]


def test_upload_paths_decide_through_the_shared_gate():
    """Four hand-written upload sessions had drifted: the follower started a
    flash erase on a non-hex md5, the Master echoed a stale rejection, Rescue
    had no stall watchdog."""
    assert not _offenders(r"inline bool normalizeOtaMd5|"
                          r"\b\w*[Rr]ejectionStatus\b|"
                          r"OTA_STALL_TIMEOUT_MS\s*=|>\s*30000UL")
    for path in UPLOAD_PATHS:
        body = _strip_comments((V2 / path).read_text())
        for call in ("otaUploadGate(", "otaUploadCompletion(", ".take(",
                     "otaUploadStalled("):
            assert call in body, f"{path}: {call}"
        # Nothing may touch the flash before the gate has passed.
        upload = body[body.index("otaUploadGate("):]
        assert "Update.begin(" in upload or "factoryWriteBegin(" in upload, path
    master = _strip_comments((V2 / "Master/WebFirmware.cpp").read_text())
    assert master.count("otaUploadGate(") == 2, "app slot and factory slot"
    for begin in ("Update.begin(", "factoryWriteBegin("):
        assert master.index(begin) > master.index("otaUploadGate("), begin


def test_every_row_master_refuses_an_upload_during_a_unit_reflash():
    body = _strip_comments((V2 / "Master/WebFirmware.cpp").read_text())
    gate = body[body.index("otaUploadGate("):]
    assert "reflashInProgress(" in gate[:gate.index(";")]
    # The row asks its unit-job module, which also counts a job still queued.
    body = _strip_comments((V2 / "FollowerEsp01/FollowerWeb.cpp").read_text())
    gate = body[body.index("otaUploadGate("):]
    assert "unitUpdateQueuedOrRunning()" in gate[:gate.index(";")]
    jobs = _strip_comments((V2 / "FollowerEsp01/FollowerUnitJobs.cpp").read_text())
    rule = jobs[jobs.index("bool unitUpdateQueuedOrRunning() {"):]
    assert "reflashInProgress(reflashProgress)" in rule[:rule.index("\n}\n")]


# --- #535: python build helpers -----------------------------------------------

BUILD_SCRIPTS = [V2 / t / "build_assets.py"
                 for t in ("Master", "FollowerEsp01", "Rescue")]


def test_build_scripts_share_one_helper_module():
    """Three build_assets.py repeated the hex parser, the rev stamping and
    the array emitter; the parser the bundle gate hashed with was a fourth."""
    owned = re.compile(r"^def (parse_intel_hex|ihex_to_image|pad_to_page|"
                       r"emit_array|compress_asset|git_short_rev|version_tag|"
                       r"bundled_unit_\w+|build_version_header)\(", re.M)
    for script in BUILD_SCRIPTS:
        text = script.read_text()
        assert "from fwbuild import" in text, script
        assert not owned.findall(text), f"{script}: {owned.findall(text)}"
    manifest = (REPO / "flashing/flasher/make_manifest.py").read_text()
    assert "from fwbuild import ihex_to_image" in manifest
    assert "def ihex_to_image" not in manifest


def test_the_asyncweb_patch_exists_once():
    assert (V2 / "buildtools/patch_asyncweb.py").is_file()
    for tree in ("Master", "FollowerEsp01", "Rescue"):
        assert not (V2 / tree / "patch_asyncweb.py").exists(), tree
        ini = (V2 / tree / "platformio.ini").read_text()
        assert "pre:../buildtools/patch_asyncweb.py" in ini, tree


# --- #537: Rescue <-> Master NVS contract, small duplicates -------------------

def test_nvs_keys_two_images_share_are_spelled_once():
    """Rescue hard-coded the namespace and keys the Master defines; a rename
    on one side would have left a rescue that cannot join WiFi."""
    contract = (SHARED / "NvsContract.h").read_text()
    literals = set(re.findall(r'#define\s+SF_NVS_\w+\s+"([^"]+)"', contract))
    assert {"splitflap", "deviceName", "wifiSsid", "wifiPass", "slotRec0",
            "slotRec1", "slotRecF"} <= literals
    quoted = "|".join(sorted(re.escape(v) for v in literals))
    hits = _offenders(r'"(?:%s)"' % quoted,
                      [V2 / "Master", V2 / "Rescue"])
    # JSON reply keys and form fields legitimately reuse some of the words.
    nvs_users = {"firmware/v2/Rescue/main.cpp",
                 "firmware/v2/Master/Settings.h",
                 "firmware/v2/Master/NvsSettingsStore.h",
                 "firmware/v2/Master/FactorySlot.cpp",
                 "firmware/v2/Master/OtaService.cpp"}
    assert not (set(hits) & nvs_users), {k: hits[k] for k in set(hits) & nvs_users}
    for path in nvs_users:
        assert "SF_NVS_" in (REPO / path).read_text(), path


def test_json_strings_are_escaped_by_one_function():
    assert not _offenders(r"inline void \w*[aA]ppendJsonString\(")
    for path in ("Master/SettingsJson.h", "FollowerEsp01/FollowerJson.h"):
        assert '#include "JsonEscape.h"' in (V2 / path).read_text(), path


def test_tests_of_shared_headers_are_not_copied_per_tree():
    """A test of a shared/ header belongs to one tree's suite; identical
    copies elsewhere only drift."""
    seen = {}
    for tree in (V2 / "Master", V2 / "FollowerEsp01", V2 / "Rescue", V2 / "Unit"):
        for test in sorted((tree / "test").glob("*/test_main.cpp")):
            seen.setdefault(test.read_bytes(), []).append(
                str(test.relative_to(REPO)))
    copies = [paths for paths in seen.values() if len(paths) > 1]
    assert not copies, copies


# --- #534: the unit-bus protocol layer -----------------------------------------

BUS_FILES = [V2 / "Master/UnitBus.cpp", V2 / "FollowerEsp01/FollowerBus.cpp"]


def test_no_tree_speaks_the_unit_protocol_by_hand():
    """Everything above the bus adapter was written twice. A unit opcode, a
    reply validator or a payload encoder in a tree means a second
    implementation of shared/UnitBusCore.h."""
    hits = _offenders(r"\bSFP_CMD_\w+|\b\w+ReadbackValid\(|"
                      r"\b(?:setOffset|setGates|setAddress|jog)Encode\(|"
                      r"\bnoArgGuardByte\(|\bbootInfoDecode\(", ROW_MASTERS)
    assert not hits, hits


def test_bus_files_touch_wire_only_in_the_adapter_and_the_probe():
    """Outside the adapter struct, the only Wire calls left are bus setup and
    the address-ACK check of a scan."""
    allowed = re.compile(r"Wire\.(?:begin|end|status|beginTransmission|"
                         r"endTransmission)\(")
    for path in BUS_FILES:
        src = _strip_comments(path.read_text())
        a = src.index("struct WireTwibootBus {")
        b = src.index("};", a)
        outside = src[:a] + src[b:]
        raw = [m for m in re.findall(r"Wire\.\w+\(", outside)
               if not allowed.fullmatch(m)]
        assert not raw, f"{path.name}: {sorted(set(raw))}"


def test_both_row_masters_run_the_shared_bootloader_sequences():
    for path in BUS_FILES:
        src = _strip_comments(path.read_text())
        for call in ("unitFlashImage(", "unitReadBootSection(",
                     "unitRescueProbe(", "unitWaitBatchIdle("):
            assert call in src, f"{path.name}: {call}"
    for path in (V2 / "Master/DisplayTask.cpp",
                 V2 / "FollowerEsp01/FollowerBus.cpp"):
        src = _strip_comments(path.read_text())
        for call in ("bootDumpRun(hooks, ", "bootUpdateRun(hooks, "):
            assert call in src, f"{path.name}: {call}"
