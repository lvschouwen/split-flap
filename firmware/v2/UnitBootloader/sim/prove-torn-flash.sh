#!/usr/bin/env bash
# #554 stopped flash — simavr proof.
#
# Runs the unit image with the hold page at address 0 (what a flash that
# stopped short leaves behind) under the units' bootloader, and the whole
# image as the control. Exit 0 iff all checks pass.
#
# Requires: the Unit firmware already built (pio run in firmware/v2/Unit).
set -euo pipefail
cd "$(dirname "$0")"
export PATH=$HOME/.platformio/packages/toolchain-atmelavr/bin:$PATH

fail() { echo "PROOF FAILED: $*" >&2; exit 1; }
trap 'fail "command failed at line $LINENO"' ERR

UNIT_HEX=../../Unit/.pio/build/unit/firmware.hex
[ -f "$UNIT_HEX" ] || fail "Unit firmware not built — run 'pio run' in firmware/v2/Unit first"

echo "=== Building twiboot"
(cd .. && python3 make_new_twiboot.py)
avr-objcopy -I ihex -O binary ../twiboot-new-atmega328p-16mhz.hex newimage.bin
avr-objcopy -I ihex -O binary "$UNIT_HEX" unit_app.bin

echo "=== Building the hold page and the harness"
g++ -std=c++17 -Wall -Werror -I ../../shared -o hold_page hold_page.cpp
./hold_page > hold_page.bin
gcc -O2 -Wall -Werror -o torn_flash_test torn_flash_test.c -lsimavr

echo "=== Running proof"
./torn_flash_test unit_app.bin newimage.bin hold_page.bin \
  || fail "torn_flash_test exited $?"

trap - ERR
echo
echo "STOPPED FLASH PROOF PASSED"
