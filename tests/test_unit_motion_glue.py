"""Source gates for the unit sketch glue around a failed home, the speed byte
and the reset cause (#502).

The decisions are pure and natively tested (UnitHomePolicy.h, UnitResetCause.h,
UnitWireContract.h); the sketch code that calls them is hardware glue no native
test reaches. These gates pin the call sites and their order, so removing one
cannot leave every suite green.
"""
import re

from test_unit_wire_glue import V2, _function_body, _strip_comments

UNIT = V2 / "Unit"


def _code(name):
    return _strip_comments((UNIT / name).read_text())


def test_a_failed_home_never_steps_on_to_the_letter():
    body = _function_body(_code("UnitMotion.ino"), "void rotateToLetter(")
    seek = body.index("calibrate(false)")
    bail = body.index("if (homingSteps < 0) {", seek)
    blind = body.index("stepFlaps(toLetter)", seek)
    assert bail < blind
    bail_body = body[bail:body.index("}", bail)]
    assert "return;" in bail_body
    # The letter stays commanded: it is what the retry shows.
    assert "receivedNumber" not in bail_body


def test_a_letter_waits_out_the_retry_gap_without_reporting_busy():
    body = _function_body(_code("UnitMotion.ino"), "void rotateToLetter(")
    gate = body.index("if (!homed && !homeAttemptAllowed(homeBackoff, millis())) {")
    # Ahead of the overheat gate: that one answers "busy", this one must not.
    assert gate < body.index("currentlyrotating = 1")
    gate_body = body[gate:body.index("}", gate)]
    assert "return;" in gate_body and "receivedNumber" not in gate_body


def test_a_low_supply_holds_a_letter_move_only_behind_its_gate():
    body = _function_body(_code("UnitMotion.ino"), "void rotateToLetter(")
    gate = body.index(
        "if (unitGateEnabled(lifetime.featureGates, UNIT_GATE_SUPPLY_WAIT)) {")
    hold = body.index(
        "if (supplyWaitHold(supplyWait, vitalsVccNow, supplyIdleLevelMv, millis())) {")
    commit = body.index("lastRotation = millis();")
    # After the overheat gate, before the move is committed and timed.
    assert body.index("OVERHEATINGTIMEOUT") < gate < hold < commit
    assert commit < body.index("extMoveStartMs = millis()")
    held = body[hold:body.index("}", hold)]
    assert "currentlyrotating = 1;" in held and "return;" in held
    # A fresh reading every time it asks, and the outcome told to the master.
    assert "vitalsSample(false);" in body[gate:hold]
    assert "EXT_DIAG_STATUS_SUPPLY_WAIT" in body[hold:commit]
    # A withdrawn letter ends the hold with the busy flag.
    loop = _function_body(_code("Unit.ino"), "void loop(")
    idle = loop.index("else if (currentlyrotating) {")
    assert "supplyWaitReset(supplyWait);" in loop[idle:idle + 500]
    # The idle level is fed by the once-a-second reading, never by the one a
    # move is judged on.
    sample = loop.index("vitalsLastSampleMs = currentMillis;")
    assert "supplyIdleLevelFold(supplyIdleLevelMv, vitalsVccNow);" in loop[sample:sample + 200]
    assert "supplyIdleLevelFold" not in body


def test_loop_pays_the_owed_retry_and_never_leaves_busy_latched():
    loop = _function_body(_code("Unit.ino"), "void loop(")
    want = loop.index(
        "if (displayedLetter != receivedNumber || homeRetryOwed(homeBackoff))")
    tail = loop[want:]
    assert "rotateToLetter(receivedNumber);" in tail
    idle = tail.index("else if (currentlyrotating) {")
    assert "currentlyrotating = 0;" in tail[idle:idle + 400]


def test_calibrate_records_both_outcomes_and_drops_homed_on_failure():
    body = _function_body(_code("UnitMotion.ino"), "int calibrate(")
    assert "recordHomeOutcome(true, true);" in body
    fail = body.index("statusLastHomeFailed = true")
    end = body.index("return -1", fail)
    failure = body[fail:end]
    assert "homed = false" in failure
    # The gap is measured from the end of the seek, after the motor stopped.
    # Whether the hall fired decides the stand-down (#667).
    assert failure.index("stopMotor()") < failure.index(
        "recordHomeOutcome(false, hallSawMagnet);")
    assert "bootHomeAttempted = true" in body


def test_a_failed_self_test_drops_homed_too():
    body = _function_body(_code("UnitMotion.ino"), "void runSelfTest(")
    failed = body[body.index("selfTest.state = SELFTEST_STATE_FAILED"):]
    failed = failed[:failed.index("unitEeRecordSelfTest")]
    assert "homed = false" in failed
    assert "recordHomeOutcome(false, selfTest.reason != SELFTEST_REASON_HALL_NEVER);" in failed
    # A self-test is a seek: the boot self-home must not add one behind it.
    assert "bootHomeAttempted = true" in body[:body.index("startMotor()")]


def test_every_seek_outcome_reaches_the_isr_copy_of_the_stand_down():
    # #667: the status reply reads statusHomeStoodDown, so the policy is only
    # ever updated through the helper that refreshes it.
    motion = _code("UnitMotion.ino")
    helper = _function_body(motion, "void recordHomeOutcome(")
    note = helper.index("homeNoteResult(homeBackoff, found, hallSeen, millis());")
    assert note < helper.index("statusHomeStoodDown = homeStoodDown(homeBackoff);")
    sketch = "".join(_code(n) for n in ("Unit.ino", "UnitMotion.ino",
                                        "UnitI2CProtocol.ino", "UnitBootUpdate.ino"))
    assert sketch.count("homeNoteResult(") == 1
    assert sketch.count("statusHomeStoodDown =") == 1
    reply = _function_body(_code("UnitI2CProtocol.ino"), "void requestEvent(")
    assert "if (statusHomeStoodDown)        flags |= UNIT_STATUS_FLAG_HOME_STOOD_DOWN;" in reply


def test_the_sketch_status_bits_match_the_masters_flag_map():
    health = (V2 / "shared/UnitHealth.h").read_text()
    sketch = _code("Unit.ino")
    for unit_name, master_name in (("UNIT_STATUS_FLAG_HOMED", "UNIT_FLAG_HOMED"),
                                   ("UNIT_STATUS_FLAG_HOME_STOOD_DOWN",
                                    "UNIT_FLAG_HOME_STOOD_DOWN")):
        mine = re.search(rf"#define {unit_name} (\(1 << \d\))", sketch).group(1)
        theirs = re.search(rf"#define {master_name}\s+(\(1 << \d\))", health).group(1)
        assert mine == theirs, (unit_name, mine, theirs)


def test_home_command_asks_before_seeking():
    body = _function_body(_code("Unit.ino"), "void loop(")
    drain = body[body.index("if (pendingHome) {"):]
    drain = drain[:drain.index("drainJog();")]
    assert drain.index("homeCommandAllowed(homeBackoff, millis())") < drain.index(
        "calibrate(true)")


def test_speed_byte_and_letter_reply_go_through_the_contract():
    proto = _code("UnitI2CProtocol.ino")
    assert "stepperSpeed = unitClampSpeed(Wire.read());" in proto
    assert not re.search(r"stepperSpeed\s*=(?!\s*unitClampSpeed\()", proto)
    reply = _function_body(proto, "void requestEvent(")
    assert "letterReplyIndex((uint8_t)displayedLetter, homed)" in reply


def test_every_requested_reset_leaves_its_marker_first():
    loop = _function_body(_code("Unit.ino"), "void loop(")
    arms = [m.start() for m in re.finditer(r"wdt_enable\(WDTO_15MS\)", loop)]
    assert arms, "the sketch no longer resets itself through the watchdog here"
    for arm in arms:
        assert "markResetRequested();" in loop[max(0, arm - 120):arm]
    # No other file arms a short watchdog behind the marker's back, except the
    # boot-update core, whose caller brackets it.
    for path in UNIT.glob("*.ino"):
        if path.name == "Unit.ino":
            continue
        assert "wdt_enable(" not in _strip_comments(path.read_text()), path.name
    update = _function_body(_code("UnitBootUpdate.ino"), "void runBootUpdate(")
    mark = update.index("if (stage == 1) markResetRequested();")
    run = update.index("bootRunStage(", mark)
    assert run < update.index("if (stage == 1) clearResetRequested();", run)


def test_boot_reads_the_cause_from_both_registers_and_consumes_the_marker():
    setup = _function_body(_code("Unit.ino"), "void setup(")
    flags = setup.index("uint8_t resetFlags = MCUSR | GPIOR0;")
    # Cleared once read: a restart that is not a reset must not see it again.
    assert "GPIOR0 = 0;" in setup[flags:flags + 400]
    read = setup.index("bool resetRequested = unitEeResetMarkRequested(mark);")
    assert read < setup.index("if (resetRequested) clearResetRequested();")
    assert "unitResetClassify(resetFlags, resetRequested)" in setup
    assert "resetKind == UNIT_RESET_BROWNOUT" in setup
    assert "resetKind == UNIT_RESET_WATCHDOG" in setup
    assert "resetStatusByte," in _function_body(
        _code("UnitI2CProtocol.ino"), "void requestEvent(")


def test_loop_runs_its_drains_and_ticks_in_their_order():
    # loop() is a sequence of drains and ticks whose order is behaviour: the
    # gates write drains ahead of the home drain so the master's GET_LIFETIME
    # read-back does not land mid-calibrate() (#409), every mutation drains
    # before the timers read currentMillis, the boot report refreshes before
    # sleep, and the letter move comes last.
    loop = _function_body(_code("Unit.ino"), "void loop(")
    steps = [
        "twiHealTick();",
        "twiDeafTick();",
        "busSilenceLoop();",
        "driftRefreshReplyBuffers();",
        "vitalsRefreshReplyBuffer();",
        "refreshExtDiagReply();",
        "clearCrashRecordOnceStable();",
        "if (pendingBootloader) {",
        "drainOffsetWrite();",
        "drainGatesWrite();",
        "drainBootUpdate();",
        "if (pendingHome) {",
        "drainJog();",
        "drainSetAddress();",
        "drainClearAddress();",
        "drainOdometerReset();",
        "persistOdometerIfDue();",
        "drainSelfTest();",
        "drainIdentify();",
        "unsigned long currentMillis = millis();",
        "identifyBlinkTick(currentMillis);",
        "uptimeTick(currentMillis);",
        "bootHomeFallbackTick(currentMillis);",
        "vitalsSample(false);",
        "dutyWindowDecayTick(currentMillis);",
        "idleHallTick(currentMillis);",
        "driftRehomeTick(currentMillis);",
        "refreshBootInfoReply();",
        "sleep_cpu();",
        "rotateToLetter(receivedNumber);",
        "currentlyrotating = 0;",
    ]
    at = []
    for step in steps:
        assert loop.count(step) == 1, f"{step} must appear once in loop()"
        at.append(loop.index(step))
    order = sorted(range(len(steps)), key=lambda i: at[i])
    assert order == list(range(len(steps))), [steps[i] for i in order]
