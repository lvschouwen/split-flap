# use_custom_bootloader.py — post extra_script (#201).
#
# pioarduino's custom_sdkconfig rebuilds app-side IDF libs only; the
# bootloader in FLASH_EXTRA_IMAGES is always the PREBUILT stock Arduino one,
# so bootloader-side options (factory reset on GPIO 4, #193) silently never
# reach the device. This script swaps the 0x0 image for the factory-reset
# bootloader built by ../Bootloader (committed under its dist/), which makes
# `pio run -t upload` and firmware.factory.bin ship it. It fails the build
# loudly if the artifact is missing — falling back to the stock bootloader
# would resurrect exactly the silent no-op this fixes.
#
# OTA'd boards are NOT covered (the bootloader is immutable over OTA):
# one-time `esptool --chip esp32s3 write_flash 0x0 <bin>` per board.

Import("env")

import os

BOOTLOADER = os.path.normpath(
    os.path.join(
        env.subst("$PROJECT_DIR"),
        "..",
        "Bootloader",
        "dist",
        "bootloader-splitflap-esp32s3.bin",
    )
)

if not os.path.isfile(BOOTLOADER):
    raise SystemExit(
        "use_custom_bootloader.py: %s is missing — build it from "
        "firmware/v2/Bootloader (see its platformio.ini)" % BOOTLOADER
    )

images = []
replaced = False
for offset, image in env.get("FLASH_EXTRA_IMAGES", []):
    if str(offset) in ("0x0000", "0x0", "0"):
        images.append((offset, BOOTLOADER))
        replaced = True
    else:
        images.append((offset, image))

if not replaced:
    raise SystemExit(
        "use_custom_bootloader.py: no bootloader entry at offset 0x0 in "
        "FLASH_EXTRA_IMAGES — pioarduino build script layout changed?"
    )

env.Replace(FLASH_EXTRA_IMAGES=images)
print("use_custom_bootloader: 0x0 -> %s" % BOOTLOADER)
