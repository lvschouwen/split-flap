"""Source gates for the rescue image's release job (#583).

The job and an upload write the same slot through the one Update session, so
each must refuse while the other runs; and the build must carry what an HTTPS
connection to the release site needs.
"""
import re
from pathlib import Path

TREE = Path(__file__).resolve().parent.parent


def _code(name):
    return re.sub(r"//[^\n]*", "", (TREE / name).read_text())


def _body(code, start):
    """From `start` to the end of the statement that closes at column 0 or 2."""
    at = code.index(start)
    return code[at:code.index("\n  });", at)]


def test_an_upload_is_refused_while_a_release_is_installed():
    code = _code("RescueWeb.cpp")
    upload = code[code.index('"/firmware/master", HTTP_POST'):code.index('"/rescue/exit"')]
    gate = upload.index("rescueReleaseInstalling()")
    # Before anything is begun for the upload.
    assert gate < upload.index("Update.begin(")
    assert "otaRejection.set(409" in upload[gate:upload.index("Update.begin(")]


def test_a_release_job_is_refused_while_an_upload_runs():
    code = _code("RescueWeb.cpp")
    ask = code[code.index("static void handleReleaseAsk("):code.index("struct SlotProbe")]
    assert "masterOtaOwnerRequest != nullptr" in ask
    assert "rescueReleaseRunning()" in ask
    # The CSRF rule comes first, as on every route that changes something.
    assert ask.index("rescueUploadCsrfRejected(request)") < ask.index("rescueReleaseAsk(")


def test_both_release_routes_go_through_the_one_gate():
    code = _code("RescueWeb.cpp")
    for route in ("/rescue/release/look", "/rescue/release/install"):
        assert "handleReleaseAsk(request" in _body(code, f'"{route}", HTTP_POST')


def test_exit_is_refused_while_a_slot_is_written():
    code = _code("RescueWeb.cpp")
    exit_route = _body(code, '"/rescue/exit", HTTP_POST')
    assert exit_route.index("rescueReleaseInstalling()") < exit_route.index("esp_ota_set_boot_partition")


def test_the_job_takes_the_site_and_the_order_from_the_shared_release_code():
    job = _code("RescueReleaseJob.cpp")
    assert '#include "ReleaseTarget.h"' in job
    # No connection, signature check or hash of its own.
    for own in ("esp_http_client", "mbedtls_", "HTTPClient"):
        assert own not in job


def test_the_build_puts_tls_memory_in_psram_and_gives_loop_the_stack():
    ini = (TREE / "platformio.ini").read_text()
    sdk = ini[ini.index("custom_sdkconfig ="):ini.index("custom_component_remove")]
    assert re.search(r"^\s+CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y$", sdk, re.M)
    assert "SET_LOOP_TASK_STACK_SIZE(16 * 1024)" in _code("main.cpp")
