"""Source gate for the WiFi-over-USB glue (#601).

ImprovPolicy.h decides (natively tested). What no native test reaches: that
the serial side hands credentials to WifiService and does nothing else with
them, that the answer owed across the restart lives where a restart leaves it,
and that a packet cannot land in the middle of a log line.
"""
import re
from pathlib import Path

TREE = Path(__file__).resolve().parent.parent


def _code(name):
    return re.sub(r"//[^\n]*", "", (TREE / name).read_text())


def _body(code, signature):
    start = code.index(signature)
    i = code.index("{", start)
    depth = 0
    for j in range(i, len(code)):
        depth += {"{": 1, "}": -1}.get(code[j], 0)
        if depth == 0:
            return code[i + 1:j]
    raise AssertionError(signature)


def test_the_serial_side_never_joins_or_stores_by_itself():
    code = _code("ImprovService.cpp")
    assert not re.search(r"\bWiFi\s*\.", code)
    assert "Preferences" not in code and "SettingsStore" not in code
    assert "saveWifiCredentials" not in code
    assert code.count("wifiStagePortalConfig(") == 1


def test_the_owed_answer_survives_the_restart_that_saves_the_credentials():
    code = _code("ImprovService.cpp")
    assert "RTC_NOINIT_ATTR static ImprovPendingRecord pending;" in code
    assert "owedPort = improvPendingDecode(pending);" in _body(code, "void improvServiceInit(")
    act = _body(code, "static void act(")
    assert act.index("wifiStagePortalConfig(") < act.index("improvPendingEncode(pending")
    tick = _body(code, "void improvServiceTick(")
    assert tick.index("writePackets(owedPort, out);") < tick.index("improvPendingClear(pending);")


def test_a_packet_is_written_whole_under_the_log_lock():
    code = _code("ImprovService.cpp")
    write = _body(code, "static void writePackets(")
    assert write.index("SerialPrintLock lock;") < write.index("->write(")
    assert code.count("->write(") == 1


def test_the_net_task_ticks_it_after_the_wifi_tick():
    tick = _body(_code("Tasks.cpp"), "static void netTaskMain(")
    assert tick.index("wifiServiceTick();") < tick.index("improvServiceTick();")


def test_the_uart_port_opens_before_the_start_up_delay_and_is_never_waited_for():
    setup = _body(_code("main.cpp"), "void setup(")
    assert setup.index("unitCatchAtPowerOn();") < setup.index("uartLogBegin();") < setup.index("delay(")
    log = _code("UartLog.h")
    write = _body(log, "inline void uartLogWrite(")
    assert write.index("if (!uartLogHasRoom(len)) return;") < write.index("Serial0.write(")
    # A full buffer reports the hardware FIFO's room, not 0.
    assert "room > SOC_UART_FIFO_LEN && room >= len + UART_LOG_SLACK" in _body(log, "inline bool uartLogHasRoom(")


def test_an_answer_waits_for_room_on_the_uart_port_before_the_lock_never_under_it():
    write = _body(_code("ImprovService.cpp"), "static void writePackets(")
    assert write.index("while (!uartLogHasRoom(total))") < write.index("SerialPrintLock lock;")
