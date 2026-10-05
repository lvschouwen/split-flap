#!/usr/bin/env bash
# #542 crash recovery — simavr proof.
#
# Builds the new twiboot (with crash record logic), a minimal crash app that
# always WDT-timeouts, and the simavr harness, then runs the five test cases.
# Exit 0 iff all checks pass.
set -euo pipefail
cd "$(dirname "$0")"
export PATH=$HOME/.platformio/packages/toolchain-atmelavr/bin:$PATH

fail() { echo "PROOF FAILED: $*" >&2; exit 1; }
trap 'fail "command failed at line $LINENO"' ERR

# 1. Build twiboot (new image with crash record logic).
echo "=== Building twiboot"
(cd .. && python3 make_new_twiboot.py)
avr-objcopy -I ihex -O binary ../twiboot-new-atmega328p-16mhz.hex newimage.bin

# 2. Build the crash app (always enables WDT-15ms then infinite-loops).
#    --defsym=__stack=0x8FD matches the real unit firmware: the crash record
#    at 0x08FE/0x08FF is above the stack and survives WDT resets.
echo "=== Building crash app"
avr-gcc -mmcu=atmega328p -Os \
  -Wl,--defsym=__stack=0x8FD \
  -o crash_app.elf crash_app.c

# Verify the crash app's BSS does not reach the crash record.
BSS_END=$(avr-nm crash_app.elf | awk '/ _end$/{print "0x" substr($1,5)}')
echo "  crash_app BSS end: $BSS_END (must be < 0x08FE)"
[ "$BSS_END" != "" ] || fail "cannot find _end symbol"
if [ "$((BSS_END))" -ge "$((0x08FE))" ]; then
  fail "crash_app BSS extends to $BSS_END, overlapping the crash record"
fi

# 3. Build and run the harness.
echo "=== Building harness"
gcc -O2 -Wall -Werror -o crash_test crash_test.c -lsimavr

echo "=== Running proof"
echo
./crash_test crash_app.elf newimage.bin || fail "crash_test exited $?"

trap - ERR
echo
echo "CRASH RECOVERY PROOF PASSED"
