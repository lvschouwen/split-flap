"""TWDT config guard (#314).

The effective ESP_TASK_WDT_TIMEOUT_S lives in the generated (gitignored)
sdkconfig; the tracked source is platformio.ini's custom_sdkconfig. This
test pins that source so a future edit can't silently drop the 30 s override
or the panic-reboot behaviour the unattended wall depends on.

Run with:
    pytest tests/
"""

from __future__ import annotations

import pathlib
import re

PROJECT_DIR = pathlib.Path(__file__).resolve().parent.parent
INI_PATH = PROJECT_DIR / "platformio.ini"


def custom_sdkconfig_lines() -> list[str]:
    """Return the entries of the [env:master] custom_sdkconfig block."""
    text = INI_PATH.read_text()
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
