"""Entry point: banner + menu loop."""
import sys

from flasher import ui, wiring
from flasher.assets import ManifestError, load_manifest


def _wiring_help(manifest):
    names = list(wiring.DIAGRAMS)
    ui.heading("Wiring help")
    for i, name in enumerate(names, 1):
        print(f" {i}. {name}")
    choice = ui.ask_int("Which diagram?", 1, len(names))
    print(wiring.DIAGRAMS[names[choice - 1]])
    ui.pause()


def main() -> None:
    ui.enable_ansi()
    try:
        manifest = load_manifest()
    except ManifestError as e:
        ui.fail(str(e))
        sys.exit(2)

    # Imported here so a broken serial stack still lets wiring help open.
    from flasher import wizard

    menu = [
        ("Provision a new display (guided, start here)", wizard.run_wizard),
        ("Prepare Arduino-as-ISP programmer", wizard.run_programmer_prep),
        ("Flash twiboot to a single unit", wizard.run_single_unit),
        ("Flash master firmware (USB serial)", wizard.run_master_serial),
        ("Update master firmware (WiFi/OTA)", wizard.run_master_ota),
        ("Check display status", wizard.run_status),
        ("Wiring help", _wiring_help),
    ]
    while True:
        ui.heading(f"SPLIT-FLAP FLASHER  (firmware {manifest['git_rev']}, built {manifest['build_date']})")
        for i, (label, _) in enumerate(menu, 1):
            print(f" {i}. {label}")
        print(" 0. Exit")
        choice = ui.ask_int("Choose", 0, len(menu))
        if choice == 0:
            return
        try:
            menu[choice - 1][1](manifest)
        except KeyboardInterrupt:
            ui.warn("interrupted — back to menu (session state saved where applicable)")
