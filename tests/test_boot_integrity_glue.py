"""Source gates for the boot-section integrity watch (#520).

The schedule, the verdict and the log edge are pure and natively tested
(BootIntegrity.h). What no native test reaches is the glue: the unit loop that
rebuilds its boot report on the timer, and the two row masters that read and
judge it on every health poll. Removing any of those leaves every suite green
and the watch silently off, so the call sites are pinned here.
"""
import re

from test_unit_wire_glue import V2, _function_body, _strip_comments


def _code(path):
    return _strip_comments((V2 / path).read_text())


def test_unit_loop_rebuilds_the_boot_report_on_the_timer_while_parked():
    loop = _function_body(_code("Unit/Unit.ino"), "void loop(")
    gate = loop.index("if (bootInfoRefreshDue(currentMillis, bootInfoLastRefreshMs,")
    body = loop[gate:loop.index("}", gate)]
    # Parked means not moving AND no letter waiting to be shown.
    assert "currentlyrotating == 0" in body
    assert "displayedLetter == receivedNumber" in body
    assert "bootInfoLastRefreshMs = currentMillis;" in body
    assert "refreshBootInfoReply();" in body
    # Ahead of the sleep block: a sleeping loop pass must not skip it.
    assert gate < loop.index("sleep_cpu();")


def test_the_refresh_recomputes_the_crc_from_flash():
    ino = _code("Unit/UnitBootUpdate.ino")
    refresh = _function_body(ino, "void refreshBootInfoReply(")
    assert "bootReadFacts()" in refresh
    assert "bootClassify(f)" in refresh


def _poll_sites(path, call):
    return len(re.findall(re.escape(call), _code(path)))


def test_both_row_masters_judge_the_report_on_probe_and_on_every_health_poll():
    # The read, the verdict and the log edge are shared/UnitBusCore.h, natively
    # tested; what is pinned here is that both the probe and the poll of each
    # row master go through the composed reads that include it.
    core = _code("shared/UnitBusCore.h")
    verdict = _function_body(core, "inline bool unitRefreshBootVerdict(")
    assert "fact.bootVerdict = BOOT_INTEGRITY_UNREAD;" in verdict
    assert "bootIntegrityJudge(report, BOOT_CURRENT_CRC32)" in verdict
    assert "bootIntegrityEdge(logged, fact.bootVerdict)" in verdict
    diagnostics = _function_body(core, "inline void unitRefreshDiagnostics(")
    assert "unitRefreshBootVerdict(bus, fact, i2cAddress, bootLogged, report)" in diagnostics
    for composed in ("inline bool unitProbeSketchUnit(", "inline bool unitPollHealth("):
        assert "unitRefreshDiagnostics(" in _function_body(core, composed), composed
    for path, probe, poll in (
            ("Master/UnitBus.cpp", "void unitBusProbe(", "bool unitBusPollHealthOne("),
            ("FollowerEsp01/FollowerBus.cpp", "void busProbeQuiet(", "bool busPollHealthOne(")):
        src = _code(path)
        assert "unitProbeSketchUnit(unitBus, unitBusNotes" in _function_body(src, probe), path
        assert "unitPollHealth(unitBus, unitBusNotes" in _function_body(src, poll), path


def test_a_corrupt_bootloader_counts_as_faulty():
    health = _code("shared/UnitHealth.h")
    fn = _function_body(health, "inline bool unitIsFaultyOrLost(")
    assert "if (u.bootVerdict == BOOT_INTEGRITY_CORRUPT) return true;" in fn


def test_a_slot_that_is_no_longer_polled_drops_its_verdict():
    # Ahead of the drivable gate on both boards: a unit that left the sketch
    # (bootloader, silent) must not keep counting as faulty on an old verdict.
    for path, sig in (("Master/UnitBus.cpp", "bool unitBusPollHealthOne("),
                      ("FollowerEsp01/FollowerBus.cpp", "bool busPollHealthOne(")):
        poll = _function_body(_code(path), sig)
        clear = poll.index("bootVerdict = BOOT_INTEGRITY_UNREAD;")
        assert clear < poll.index("return false;"), path
