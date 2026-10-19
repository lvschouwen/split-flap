"""TWDT config guard (#314).

The effective ESP_TASK_WDT_TIMEOUT_S is what the shared Arduino IDF libs were
compiled with; the tracked source is platformio.ini's custom_sdkconfig. This
test pins that source so a future edit can't silently drop the 30 s override
or the panic-reboot behaviour the unattended wall depends on. That the libs
carry it is checked at build time (../buildtools/check_framework_libs.py, #634).

Run with:
    pytest tests/
"""

from __future__ import annotations

import pathlib
import re

PROJECT_DIR = pathlib.Path(__file__).resolve().parent.parent
INI_PATH = PROJECT_DIR / "platformio.ini"


def custom_sdkconfig_lines(ini: pathlib.Path = INI_PATH) -> list[str]:
    """Return the entries of a project's custom_sdkconfig block."""
    text = ini.read_text()
    m = re.search(r"^custom_sdkconfig\s*=\s*$(.*?)^\S", text,
                  re.MULTILINE | re.DOTALL)
    assert m, "custom_sdkconfig block not found in platformio.ini"
    lines = []
    for raw in m.group(1).splitlines():
        stripped = raw.strip()
        if stripped and not stripped.startswith(";"):
            lines.append(stripped)
    return lines


def test_task_wdt_timeout_is_30s():
    assert "CONFIG_ESP_TASK_WDT_TIMEOUT_S=30" in custom_sdkconfig_lines()


def test_rescue_compiles_the_same_framework_libs():
    # #634: Master and Rescue compile one shared Arduino IDF lib build; the one
    # that builds last decides what the other links. Equal blocks keep either
    # order right; check_framework_libs.py catches what slips past this.
    rescue = INI_PATH.parent.parent / "Rescue" / "platformio.ini"
    assert custom_sdkconfig_lines(rescue) == custom_sdkconfig_lines()


def test_both_builds_check_the_shared_libs():
    for ini in (INI_PATH, INI_PATH.parent.parent / "Rescue" / "platformio.ini"):
        assert "pre:../buildtools/check_framework_libs.py" in ini.read_text(), ini


def test_task_wdt_panic_not_disabled():
    # PANIC defaults on (framework) and must not be turned off here — a
    # timeout must reboot, not just warn.
    assert "CONFIG_ESP_TASK_WDT_PANIC=n" not in custom_sdkconfig_lines()


# ---- #630: no app task may wait on the network for longer than the TWDT ----

MQTT_TRANSPORT = PROJECT_DIR / "MqttTransport.cpp"


def test_mqtt_writes_hand_the_socket_only_what_it_takes_now():
    # espMqttClient's own transport waits in NetworkClient::write with a budget
    # that starts over at every partial send; one mqtt pass then outlasts the
    # 30 s TWDT on a link that drips. The wall's transport never waits.
    text = MQTT_TRANSPORT.read_text()
    start = text.index("MqttNonBlockingTransport::write")
    end = text.index("MqttWallClient::MqttWallClient")
    write = text[start:end]
    assert "MSG_DONTWAIT" in write
    for waiting in ("select", "client.write", "delay(", "vTaskDelay"):
        assert waiting not in write, f"the mqtt write must not wait: {waiting}"


def test_mqtt_service_runs_on_the_non_waiting_client():
    service = (PROJECT_DIR / "MqttService.cpp").read_text()
    assert re.search(r"^static MqttWallClient mqttClient;", service, re.M)
    assert not re.search(r"^static espMqttClient\b", service, re.M), \
        "the stock espMqttClient waits in its writes (#630)"


def test_crash_report_names_the_task_the_watchdog_named():
    # The RTC crash slots cannot name the task the TWDT tripped on; the dump's
    # panic reason can. Both the boot log and the summary route carry it.
    assert "coredumpPanicReason(" in (PROJECT_DIR / "main.cpp").read_text()
    web = (PROJECT_DIR / "WebSystem.cpp").read_text()
    assert "panicReason" in web
    assert "esp_core_dump_get_panic_reason(" in web
