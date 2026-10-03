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
    assert "homeNoteResult(homeBackoff, true, millis())" in body
    fail = body.index("statusLastHomeFailed = true")
    end = body.index("return -1", fail)
    failure = body[fail:end]
    assert "homed = false" in failure
    # The gap is measured from the end of the seek, after the motor stopped.
    assert failure.index("stopMotor()") < failure.index(
        "homeNoteResult(homeBackoff, false, millis())")
    assert "bootHomeAttempted = true" in body


def test_a_failed_self_test_drops_homed_too():
    body = _function_body(_code("UnitMotion.ino"), "void runSelfTest(")
    failed = body[body.index("selfTest.state = SELFTEST_STATE_FAILED"):]
    failed = failed[:failed.index("unitEeRecordSelfTest")]
    assert "homed = false" in failed
    assert "homeNoteResult(homeBackoff, false, millis())" in failed
    # A self-test is a seek: the boot self-home must not add one behind it.
    assert "bootHomeAttempted = true" in body[:body.index("startMotor()")]


def test_home_command_asks_before_seeking():
    body = _function_body(_code("Unit.ino"), "void loop(")
    drain = body[body.index("if (pendingHome) {"):]
    drain = drain[:drain.index("if (pendingJogSteps != 0)")]
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
