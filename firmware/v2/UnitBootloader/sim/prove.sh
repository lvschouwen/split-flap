#!/usr/bin/env bash
# #499 in-system twiboot update — simavr proof against the REAL new twiboot image.
#
# Stage 1: drive the fielded twiboot's write handler to install the real do_spm
#          (+ ABI marker) into its empty page 7.
# Stage 2: call that do_spm to rewrite pages 0-6 with the real new twiboot.
#
# Both stages are checked byte-for-byte against the expected boot section.
set -euo pipefail
cd "$(dirname "$0")"
export PATH=$HOME/.platformio/packages/toolchain-atmelavr/bin:$PATH

# 1. Build the new image + generated header (writes sim/new_image.h).
(cd .. && python3 make_new_twiboot.py)

# 2. Extract the fielded twiboot and the new image as raw boot-section binaries.
avr-objcopy -I ihex -O binary ../prebuilt/twiboot-atmega328p-16mhz.hex fielded.bin
avr-objcopy -I ihex -O binary ../twiboot-new-atmega328p-16mhz.hex newimage.bin

python3 - <<'PY'
# Pad fielded to 1024 (its page 7 is blank/absent in the hex).
fielded = bytearray(open('fielded.bin', 'rb').read().ljust(1024, b'\xff'))
new = open('newimage.bin', 'rb').read()
assert len(new) == 1024
# Stage-1 expected: fielded pages 0-6 unchanged, page 7 replaced by new page 7.
s1 = bytearray(fielded)
s1[0x380:0x400] = new[0x380:0x400]
open('expect_stage1.bin', 'wb').write(s1)
# Stage-2 overlay (post-stage-1 state): same as stage-1 expected.
open('overlay_stage2.bin', 'wb').write(s1)
# Stage-2 expected: the full new image.
open('expect_stage2.bin', 'wb').write(new)
print('built expected/overlay bins')
PY

# 3. Compile the harness and the two updater test apps.
gcc runtest.c -o runtest -lsimavr
avr-gcc -Os -mmcu=atmega328p -I. -o drivefill.elf drivefill.c
avr-gcc -Os -mmcu=atmega328p -I. -o stage2.elf stage2.c

sym() { avr-nm "$1" | awk "/ $2\$/{print \"0x\"\$1}"; }
dsym() { avr-nm "$1" | awk "/ $2\$/{print \"0x\"substr(\$1,5)}"; }

echo "=== STAGE 1: install real do_spm into page 7 (expect full match) ==="
./runtest drivefill.elf fielded.bin "$(sym drivefill.elf main)" 2000000 \
  "$(dsym drivefill.elf done)" expect_stage1.bin
echo "=== STAGE 2: rewrite pages 0-6 with real new twiboot (expect full match) ==="
./runtest stage2.elf overlay_stage2.bin "$(sym stage2.elf main)" 2000000 \
  "$(dsym stage2.elf done)" expect_stage2.bin
echo "ALL STAGES PASSED"
