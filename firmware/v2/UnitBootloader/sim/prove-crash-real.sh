#!/usr/bin/env bash
# #542 crash recovery — real-application proof.
#
# Three cases the stand-in crash_app can't cover:
#   (a) Real unit app hang → held on 3rd WDT
#   (b) Real unit app intentional reboots → never held
#   (d) Fielded app (stack at RAMEND) → never held
#
# Requires: the Unit firmware already built (pio run in firmware/v2/Unit).
set -euo pipefail
cd "$(dirname "$0")"
export PATH=$HOME/.platformio/packages/toolchain-atmelavr/bin:$PATH

fail() { echo "PROOF FAILED: $*" >&2; exit 1; }
trap 'fail "command failed at line $LINENO"' ERR

UNIT_ELF=../../Unit/.pio/build/unit/firmware.elf
[ -f "$UNIT_ELF" ] || fail "Unit firmware not built — run 'pio run' in firmware/v2/Unit first"

# 1. Build twiboot image.
echo "=== Building twiboot"
(cd .. && python3 make_new_twiboot.py)
avr-objcopy -I ihex -O binary ../twiboot-new-atmega328p-16mhz.hex newimage.bin

# 2. Build the "fielded" app: same as crash_app but WITHOUT --defsym=__stack.
#    Stack starts at RAMEND (0x08FF), which is where the crash record lives.
#    This simulates any unit on a pre-#542 firmware.
echo "=== Building fielded (no --defsym) app"
avr-gcc -mmcu=atmega328p -Os \
  -o fielded_app.elf crash_app.c

BSS_END=$(avr-nm fielded_app.elf | awk '/ _end$/{print "0x" substr($1,5)}')
STACK=$(avr-nm fielded_app.elf | awk '/ __stack$/{print "0x" substr($1,5)}')
echo "  fielded_app: BSS end=$BSS_END, __stack=${STACK:-RAMEND (default)}"
if [ -n "$STACK" ] && [ "$STACK" != "0x08ff" ]; then
  fail "fielded_app has __stack=$STACK, expected RAMEND (0x08ff)"
fi

# 3. Build and run the harness.
echo "=== Building harness"
gcc -O2 -Wall -Werror -o crash_test_real crash_test_real.c -lsimavr

echo "=== Running proof"
echo
./crash_test_real "$UNIT_ELF" fielded_app.elf newimage.bin \
  || fail "crash_test_real exited $?"

trap - ERR
echo
echo "REAL-APP CRASH RECOVERY PROOF PASSED"
