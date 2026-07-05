#!/usr/bin/env bash
# Rebuild the prebuilt/ binaries from the current source tree and package the
# whole flashing/ directory into /tmp/split-flap-flasher.zip. Run this before
# shipping a new release zip.
#
# Requires PlatformIO installed (`pio` on PATH) and a working AVR toolchain
# (PlatformIO pulls it in automatically when you `pio run` the Unit sketch).

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FLASHING_DIR="$REPO_ROOT/flashing"
PREBUILT_DIR="$FLASHING_DIR/prebuilt"
ZIP_OUT="/tmp/split-flap-flasher.zip"

echo "=== Building ESPMaster sketch + filesystem ==="
( cd "$REPO_ROOT/firmware/v1/ESPMaster" && pio run -e espmaster && pio run -e espmaster -t buildfs )

echo "=== Building Unit sketch ==="
( cd "$REPO_ROOT/firmware/v1/Unit" && pio run )

echo "=== Copying artefacts to flashing/prebuilt/ ==="
mkdir -p "$PREBUILT_DIR"
cp "$REPO_ROOT/firmware/v1/UnitBootloader/prebuilt/twiboot-atmega328p-16mhz.hex" "$PREBUILT_DIR/"
cp "$REPO_ROOT/firmware/v1/Unit/.pio/build/unit/firmware.hex"                   "$PREBUILT_DIR/unit-firmware.hex"
cp "$REPO_ROOT/firmware/v1/ESPMaster/.pio/build/espmaster/firmware.bin"         "$PREBUILT_DIR/master-firmware.bin"
cp "$REPO_ROOT/firmware/v1/ESPMaster/.pio/build/espmaster/littlefs.bin"         "$PREBUILT_DIR/master-littlefs.bin"

echo "=== Packaging to $ZIP_OUT ==="
STAGING="$(mktemp -d)"
trap "rm -rf '$STAGING'" EXIT
cp -r "$FLASHING_DIR" "$STAGING/split-flap-flasher"
rm -f "$STAGING/split-flap-flasher/package-flasher.sh"  # builder script isn't useful to end users
( cd "$STAGING" && zip -qr "$ZIP_OUT" split-flap-flasher/ )

echo
echo "Done: $ZIP_OUT"
ls -lh "$ZIP_OUT"
