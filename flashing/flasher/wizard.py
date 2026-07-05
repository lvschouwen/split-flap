"""The guided flows behind every menu entry."""
import time

from flasher import avr, esp, ota, ports, ui, wiring
from flasher.assets import asset_path
from flasher.session import (Session, default_session_path, load_session,
                             next_unit, save_session)


def network_verdict(settings, expected_n: int, manifest_rev: str):
    """Verify a live display against the manifest + requested unit count.

    Uses detectedUnitCount + detectedUnitAddresses — NOT unitCount, which is
    displayWidth (highest responder + 1) and reads N even with dead units.
    """
    if settings is None:
        return False, ["device unreachable — is it on your WiFi? (check router for its IP)"]
    problems = []
    rev = str(settings.get("version", "?"))
    if rev != manifest_rev:
        problems.append(f"running firmware {rev}, this flasher shipped {manifest_rev}")
    addrs = set(settings.get("detectedUnitAddresses", []))
    expected = set(range(1, expected_n + 1))
    missing = sorted(expected - addrs)
    if missing:
        problems.append(f"units missing from I2C bus (addresses): {missing}")
    detected = settings.get("detectedUnitCount", 0)
    if detected != expected_n and not missing:
        problems.append(f"detectedUnitCount {detected} != expected {expected_n}")
    return (not problems), problems


def _pick_port(purpose: str) -> str:
    while True:
        found = ports.describe_ports()
        if not found:
            ui.warn(ports.DRIVER_HINT)
            ui.pause("Fix the driver / plug the device, then press Enter to rescan...")
            continue
        ui.say(f"\nSerial ports ({purpose}):")
        for i, d in enumerate(found, 1):
            ui.say(f" {i}. {d}")
        ui.say(f" {len(found) + 1}. rescan")
        choice = ui.ask_int("Which port?", 1, len(found) + 1)
        if choice <= len(found):
            return found[choice - 1].split(" — ")[0]


def _flash_programmer(port: str) -> bool:
    avrdude, conf = avr.find_avrdude()
    hex_path = str(asset_path("ArduinoISP.hex"))
    for baud in (115200, 57600):
        ui.say(f"flashing ArduinoISP at {baud} baud...")
        code, out = avr.run_avrdude(avr.arduinoisp_args(avrdude, conf, port, baud, hex_path))
        if code == 0:
            return True
    ui.fail("could not flash ArduinoISP — output above; check the board is a stock Uno/Nano")
    ui.say(out[-2000:])
    return False


def run_programmer_prep(manifest, session=None) -> str | None:
    ui.heading("Prepare Arduino-as-ISP programmer")
    print(wiring.DIAGRAMS["programmer"])
    if not ui.ask_yn("Flash ArduinoISP onto the spare board now?", default=True):
        return None
    ui.say("Plug the spare Uno/Nano into USB (WITHOUT the 10uF cap fitted).")
    port = _pick_port("programmer board")
    if not _flash_programmer(port):
        return None
    ui.ok("programmer ready — now fit the 10uF cap (RESET->GND) and wire the 6 ICSP lines")
    print(wiring.DIAGRAMS["icsp"])
    return port


def _flash_one_unit(unit_no: int, port: str) -> bool:
    avrdude, conf = avr.find_avrdude()
    ui.heading(f"Unit {unit_no}")
    ui.say(f"  {wiring.dip_visual(unit_no)}")
    ui.say("  Disconnect the stepper, set the DIP switches, clip the 6 ICSP wires.")
    ui.pause()
    code, out = avr.run_avrdude(avr.probe_args(avrdude, conf, port))
    sig = avr.parse_signature(out)
    if sig != avr.EXPECTED_SIGNATURE:
        ui.fail(f"signature {sig or 'unreadable'} (expected {avr.EXPECTED_SIGNATURE}) — "
                "wiring/power problem; 0xffffff usually means MOSI/MISO swapped or stepper attached")
        return False
    code, out = avr.run_avrdude(avr.icsp_flash_args(avrdude, conf, port,
                                                    str(asset_path("twiboot-atmega328p-16mhz.hex"))))
    if code != 0:
        ui.fail("twiboot flash failed:\n" + out[-2000:])
        return False
    code, out = avr.run_avrdude(avr.fuse_write_args(avrdude, conf, port))
    if code != 0:
        ui.fail("fuse write failed:\n" + out[-2000:])
        return False
    code, out = avr.run_avrdude(avr.fuse_read_args(avrdude, conf, port))
    fuses = avr.parse_fuses(out)
    if not fuses or not avr.fuses_ok(*fuses):
        ui.fail(f"fuse verify failed — read {fuses}, expected L=0xff H=0xdc E=0xfd(&0x07)")
        return False
    ui.ok(f"unit {unit_no}: twiboot installed, fuses verified")
    return True


def run_wizard(manifest) -> None:
    ui.heading("Provision a new display")
    spath = default_session_path()
    session = load_session(spath)
    if session and session.unit_count and next_unit(session) is not None:
        if ui.ask_yn(f"Resume previous run ({len(session.done)}/{session.unit_count} units done)?",
                     default=True):
            pass
        else:
            session = None
    if session is None or not session.unit_count:
        session = Session(unit_count=ui.ask_int("How many units does this display have?", 1, 16))
        save_session(session, spath)

    if session.programmer_port is None:
        if ui.ask_yn("Do you already have an Arduino-as-ISP programmer?", default=False):
            print(wiring.DIAGRAMS["icsp"])
            session.programmer_port = _pick_port("programmer")
        else:
            session.programmer_port = run_programmer_prep(manifest)
            if session.programmer_port is None:
                return
        save_session(session, spath)

    while (unit := next_unit(session)) is not None:
        done_n = len(session.done)
        ui.say(f"\n--- progress: {done_n}/{session.unit_count} done, "
               f"{len(session.skipped)} skipped ---")
        if _flash_one_unit(unit, session.programmer_port):
            session.done.append(unit)
            if unit in session.skipped:
                session.skipped.remove(unit)
        else:
            action = ui.ask("(r)etry / (s)kip for now / (a)bort?").lower()
            if action == "s" and unit not in session.skipped:
                session.skipped.append(unit)
            elif action == "a":
                save_session(session, spath)
                ui.warn("aborted — run option 1 again to resume where you left off")
                return
        save_session(session, spath)

    ui.ok(f"all {session.unit_count} units done")
    run_master_serial(manifest)

    ui.heading("Assemble the display")
    print(wiring.DIAGRAMS["assembly"])
    ui.pause("Assemble + power everything, then press Enter...")
    ui.heading("First boot")
    ui.say("Join WiFi network 'split-flap-<chipid>-setup' (no password); a portal opens\n"
           "(or browse to http://192.168.4.1/). Enter your home WiFi credentials.\n"
           "The master reboots onto your WiFi and auto-installs the unit firmware over I2C.")
    if ui.ask_yn("Verify the live display over the network now?", default=True):
        run_status(manifest, expected_n=session.unit_count)


def run_single_unit(manifest) -> None:
    unit = ui.ask_int("Which unit number?", 1, 16)
    print(wiring.DIAGRAMS["icsp"])
    port = _pick_port("programmer")
    _flash_one_unit(unit, port)


def run_master_serial(manifest) -> None:
    ui.heading("Flash master (ESP-01 over USB)")
    print(wiring.DIAGRAMS["esp_uart"])
    before = set(ports.list_port_names())
    ui.say("Jumper GPIO0 to GND, then plug the USB-UART adapter in now...")
    port = ports.wait_for_new_port(before, timeout=60) or _pick_port("USB-UART adapter")
    ui.say(f"using {port}")
    try:
        warnings = esp.flash_master(port, str(asset_path("master-firmware.bin")))
    except esp.ImageError as e:
        ui.fail(str(e))
        return
    except Exception as e:  # esptool failures surface here
        ui.fail(f"esptool failed: {e}\nCommon causes: GPIO0 not grounded at power-up, "
                "wrong port, adapter can't power the ESP")
        return
    for w in warnings:
        ui.warn(w)
    ui.ok("master flashed — unplug, REMOVE the GPIO0 jumper, plug back in")


def run_master_ota(manifest) -> None:
    ui.heading("Update master over WiFi")
    target = ui.ask("Device address (e.g. http://192.168.1.50):").rstrip("/")
    quiet = ui.ask_yn("Use quiet OTA mode (display stays dark during flash)?", default=True)
    verdict = ota.run_ota(target, str(asset_path("master-firmware.bin")),
                          manifest["git_rev"], max_attempts=4, quiet_mode=quiet, say=ui.say)
    if verdict == ota.SUCCESS:
        ui.ok("SUCCESS — new firmware is running")
    elif verdict == ota.REVERTED:
        ui.fail("EBOOT SILENT REVERT after all retries — power-supply margin problem; "
                "add caps / beefier 3.3V (see CLAUDE.md OTA section)")
    else:
        ui.fail(f"verdict: {verdict} — not retryable (config/network problem, retrying can't fix it)")


def run_status(manifest, expected_n: int | None = None) -> None:
    target = ui.ask("Device address (e.g. http://192.168.1.50):").rstrip("/")
    settings = ota.fetch_settings(target)
    if settings is None:
        ui.fail("unreachable")
        return
    for key in ("version", "deviceName", "sketchMd5", "lastFlashResult",
                "unitCount", "detectedUnitCount", "detectedUnitAddresses"):
        ui.say(f"  {key:24}: {settings.get(key)}")
    if expected_n is None:
        expected_n = ui.ask_int("How many units should be present?", 1, 16)
    good, problems = network_verdict(settings, expected_n, manifest["git_rev"])
    if good:
        ui.ok(f"Display alive — {expected_n}/{expected_n} units, firmware {manifest['git_rev']}")
    else:
        for p in problems:
            ui.fail(p)
