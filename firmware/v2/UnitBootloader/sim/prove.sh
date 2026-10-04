#!/usr/bin/env bash
# #499 in-system twiboot update — simavr proof, single entry point.
#
# Runs the unit firmware's own self-program core (firmware/v2/Unit/
# BootUpdateAvr.h, compiled unchanged into updater_app.cpp) against the REAL
# fielded twiboot and the REAL new image, in the order the sketch uses it, and
# checks the whole boot section byte for byte. Any failed check exits non-zero
# with "PROOF FAILED".
set -euo pipefail
cd "$(dirname "$0")"
export PATH=$HOME/.platformio/packages/toolchain-atmelavr/bin:$PATH

fail() { echo "PROOF FAILED: $*" >&2; exit 1; }
trap 'fail "command failed at line $LINENO"' ERR

# BootUpdateResult / BootSectionState values (shared/BootUpdateReport.h,
# shared/BootSectionClassify.h).
R_NONE=0 R_S2_OK=2 R_REFUSED_STATE=3 R_VERIFY_FAILED=6
S_TRAMPOLINE=3 S_NEW=4

# 1. New image + canonical generated header (../prebuilt/twiboot-new-progmem.h).
(cd .. && python3 make_new_twiboot.py)

# 2. Boot-section images.
avr-objcopy -I ihex -O binary ../prebuilt/twiboot-atmega328p-16mhz.hex fielded.raw
avr-objcopy -I ihex -O binary ../twiboot-new-atmega328p-16mhz.hex newimage.bin
avr-objcopy -I ihex -O binary ../prebuilt/twiboot-prev-e422a668.hex prevnew.bin
avr-objcopy -I ihex -O binary ../prebuilt/twiboot-prev-081c2954.hex prevnew2.bin
python3 - <<'PY'
fielded = bytearray(open('fielded.raw', 'rb').read().ljust(1024, b'\xff'))
new = open('newimage.bin', 'rb').read()
assert len(new) == 1024
tramp = bytes([0x0C, 0x94, 0x00, 0x00]) + b'\xff' * 124   # jmp 0x0000 + pad
def w(name, b): open(name, 'wb').write(bytes(b))
w('fielded.bin', fielded)
# Post-stage-1: fielded pages 0-6 + new page 7 (do_spm).
w('page7_installed.bin', fielded[:0x380] + new[0x380:])
# Trampoline in page 0 but page 7 still blank: must never call into page 7.
w('tramp_no_dospm.bin', tramp + fielded[0x80:])
# Killed just before the final page-0 write: only page 0 still to do.
w('tramp_rest_new.bin', tramp + new[0x80:])
print('built boot-section images')
PY

# 3. Harness + the updater app (same header the sketch compiles).
gcc -O2 -Wall -Werror runtest.c -o runtest -lsimavr
avr-g++ -Os -mmcu=atmega328p -Wall -Werror -I../../Unit -I../../shared \
  -I../prebuilt -o updater_app.elf updater_app.cpp
REPORT=0x$(avr-nm updater_app.elf | awk '/ sim_report$/{print substr($1,5)}')
[ "$REPORT" != "0x" ] || fail "sim_report symbol not found"

APP=updater_app.elf
step() { echo; echo "=== $1"; shift; ./runtest "$APP" "$@" || fail "$*"; }

# SPM numbering: stage 1 = 67 operations inside twiboot (erase, 64 fills,
# write, rww); stage 2 = 8 page writes x 67 via do_spm (trampoline, pages 1-6,
# final page 0). Full run = 67 + 536 = 603.

# A. Whole update from power-on through the fielded twiboot. Stage 1 leaves
#    twiboot idling; it exits either by its own "jump to app" command byte (the
#    overlaid application RAM at 0x019D happens to read 0x21) or by the
#    watchdog reset. Its timeout countdown cannot exit (stage 1 zeroes r10, the
#    command its expiry stores) — the armed-countdown case proves that.
step "A1 full update, twiboot exits to app (cmd byte 0x21)" \
  fielded.bin newimage.bin "$REPORT" --start reset --twiboot-ram 0x21:0:0 \
  --expect 0:$R_S2_OK:$S_NEW --expect-spm 603 --expect-resets 1
step "A2 full update, watchdog reset regains control" \
  fielded.bin newimage.bin "$REPORT" --start reset --twiboot-ram 0x00:0:0 \
  --expect 0:$R_S2_OK:$S_NEW --expect-spm 603 --expect-resets 2
step "A3 full update, twiboot timeout armed: still the watchdog" \
  fielded.bin newimage.bin "$REPORT" --start reset --twiboot-ram 0x00:1:5 \
  --expect 0:$R_S2_OK:$S_NEW --expect-spm 603 --expect-resets 2

# B. Power loss before every one of the 603 flash operations (and once after
#    the last), each followed by a reset and the unit's own recovery: a retried
#    stage 1, or the trampoline booting the app which auto-resumes stage 2.
#    Every point must end byte-identical to the new image, except the points
#    where page 0 is erased and not yet rewritten — exactly the two page-0
#    windows (trampoline write, final write), 65 points each. Stage 1 exits
#    twiboot via its command byte here (A2/A3 pin the watchdog exit): it keeps
#    the sweep fast and the flash states visited are the same.
step "B  kill sweep over the whole update" \
  fielded.bin newimage.bin "$REPORT" --start app --twiboot-ram 0x21:0:0 \
  --sweep-kill 0 603 --expect-windows 130

# C. A page that does not verify stops stage 2 with the trampoline still in
#    page 0 (the unit keeps booting its app); the next boot auto-resumes.
#    Page 3's write is SPM #67*3+65; corrupt it just before its rww (#267).
step "C  page-3 verify failure -> stop resumable, resume on next boot" \
  page7_installed.bin newimage.bin "$REPORT" --start app --boots 2 \
  --corrupt-at-spm 267 0x7d85 \
  --expect 0:$R_VERIFY_FAILED:$S_TRAMPOLINE --expect 1:$R_S2_OK:$S_NEW

# D. A final page-0 write that does not verify puts the trampoline back (the
#    only page 0 a reset can boot) and the next boot resumes. Final page 0 is
#    written at SPM #534; corrupt before its rww (#535).
step "D  final page-0 verify failure -> trampoline restored, resume" \
  page7_installed.bin newimage.bin "$REPORT" --start app --boots 2 \
  --corrupt-at-spm 535 0x7c10 \
  --expect 0:$R_VERIFY_FAILED:$S_TRAMPOLINE --expect 1:$R_S2_OK:$S_NEW

# H. A trampoline write that does not verify is retried while the app is still
#    alive (a corrupt page 0 is the next reset's brick). Trampoline write is
#    SPM #65; corrupt before its rww (#66). One extra page write: 536 + 67.
step "H  trampoline verify failure -> retried in the same run" \
  page7_installed.bin newimage.bin "$REPORT" --start app \
  --corrupt-at-spm 66 0x7c00 --expect 0:$R_S2_OK:$S_NEW --expect-spm 603

# E. Trampoline with no do_spm in page 7: auto-resume must refuse without a
#    single SPM (calling into a blank page 7 would execute erased flash).
step "E  no do_spm in page 7 -> refuse, zero SPMs" \
  tramp_no_dospm.bin tramp_no_dospm.bin "$REPORT" --start reset \
  --expect 0:$R_REFUSED_STATE:$S_TRAMPOLINE --expect-spm 0

# F. Resume is idempotent: trampoline already in place and pages 1-6 already
#    new -> only the final page 0 is written (67 SPMs, no second trampoline).
step "F  resume skips the trampoline and matching pages" \
  tramp_rest_new.bin newimage.bin "$REPORT" --start reset \
  --expect 0:$R_S2_OK:$S_NEW --expect-spm 67

# P. A unit on the PREVIOUS image (what the fleet carries): a complete,
#    working bootloader whose page 7 already holds do_spm, so stage 2 alone
#    replaces it — 8 page writes x 67 = 536, no stage 1, no reset.
step "P1 previous image -> new image by stage 2 alone" \
  prevnew.bin newimage.bin "$REPORT" --start app \
  --expect 0:$R_S2_OK:$S_NEW --expect-spm 536 --expect-resets 0
#    Power loss before every one of those 536 operations: all recover through
#    the trampoline and the app's auto-resume, except the same two page-0
#    windows as in B.
step "P2 kill sweep from the previous image" \
  prevnew.bin newimage.bin "$REPORT" --start app \
  --sweep-kill 0 536 --expect-windows 130

# Q. A unit on the SECOND previous image (the one this build replaces): also a
#    complete bootloader with do_spm in page 7, same stage-2-only path.
step "Q1 second previous image -> new image by stage 2 alone" \
  prevnew2.bin newimage.bin "$REPORT" --start app \
  --expect 0:$R_S2_OK:$S_NEW --expect-spm 536 --expect-resets 0
step "Q2 kill sweep from the second previous image" \
  prevnew2.bin newimage.bin "$REPORT" --start app \
  --sweep-kill 0 536 --expect-windows 130

# G. A unit already on the new image does nothing.
step "G  new image is left alone" \
  newimage.bin newimage.bin "$REPORT" --start app \
  --expect 0:$R_NONE:$S_NEW --expect-spm 0

trap - ERR
echo
echo "ALL PROOFS PASSED"
