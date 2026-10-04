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


def test_s3_master_judges_the_report_on_probe_and_on_every_health_poll():
    src = _code("Master/UnitBus.cpp")
    fn = _function_body(src, "static void refreshUnitBootVerdict(")
    assert "fact.bootVerdict = BOOT_INTEGRITY_UNREAD;" in fn
    assert "unitBusReadBootInfo(i2cAddress, r)" in fn
    assert "bootIntegrityJudge(r, BOOT_CURRENT_CRC32)" in fn
    assert "bootIntegrityEdge(bootVerdictLogged[unitIndex], fact.bootVerdict)" in fn
    poll = _function_body(src, "bool unitBusPollHealthOne(")
    assert "refreshUnitBootVerdict(facts[i], i);" in poll
    assert src.count("refreshUnitBootVerdict(") == 3  # definition, probe, poll


def test_esp01_follower_judges_the_report_on_probe_and_on_every_health_poll():
    src = _code("FollowerEsp01/FollowerBus.cpp")
    fn = _function_body(src, "static void refreshUnitBootVerdict(")
    assert "fact.bootVerdict = BOOT_INTEGRITY_UNREAD;" in fn
    assert "busReadBootInfo(i2cAddress, r)" in fn
    assert "bootIntegrityJudge(r, BOOT_CURRENT_CRC32)" in fn
    assert "bootIntegrityEdge(bootVerdictLogged[unitIndex], fact.bootVerdict)" in fn
    poll = _function_body(src, "bool busPollHealthOne(")
    assert "refreshUnitBootVerdict(f, i);" in poll
    assert src.count("refreshUnitBootVerdict(") == 3  # definition, probe, poll


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
