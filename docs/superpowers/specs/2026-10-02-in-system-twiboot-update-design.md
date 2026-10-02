# In-system twiboot update over I2C (#499)

Status: design approved in chat 2026-10-02; implementation pending.
Stage 1 (boot-section dump, #511) already shipped.

## Goal

Replace the bootloader (twiboot) on fielded split-flap units over I2C, with no
ICSP, and leave behind a bootloader that can be updated routinely afterwards.
The update runs from the unit's own application firmware, driven one unit at a
time by the S3 master. The S3 row (16 units) goes first; the ESP-01 row (5
units) is a follow-up issue once the mechanism is proven on the wall.

## What is already known (stage 1, #511)

- All 16 S3-row units run the committed prebuilt image
  `UnitBootloader/prebuilt/twiboot-atmega328p-16mhz.hex`: 890 of 1024 bytes,
  CRC32 `18173add`. The vendored `main.c` built with the PlatformIO toolchain
  (avr-gcc 7.3.0) reproduces that image **byte for byte** — confirmed in this
  session. The new image is therefore a known delta from a known base.
- Boot section is the real NRWW section at `0x7C00`–`0x7FFF`,
  `VIRTUAL_BOOT_SECTION=0`, BOOTRST set (HFUSE `0xDC`), so reset enters the
  bootloader first. Interrupt vectors stay in the application section (IVSEL
  clear).
- **Page 7 (`0x7F80`–`0x7FFF`) is empty** (all `0xFF`).
- twiboot's SPM instructions sit at `0x7e60`, `0x7e86`, `0x7ea6`, `0x7eb2`.
  None is a clean callable `do_spm` — each is followed by a jump back into
  twiboot, not a return to the caller.
- The 5 ESP-01-row units were not dumped (the follower has no dump op); they are
  presumed on the same image and are guarded at update time by the same
  boot-section CRC check.

## Why it is safe to attempt

The only moment a unit can be bricked is while a page that is on the reset /
execution path is mid-write and power is lost. The design shrinks that window to
two ~9 ms page-0 writes per unit and makes everything else resumable:

1. SPM executes only from the boot section. The application cannot write flash
   directly; it must transfer control to code in the boot section.
2. **Stage 1** gets a proper `do_spm` into the *empty* page 7 using the
   interrupt-return trick (below). Page 7 is not on any execution path, so a
   failure here cannot brick a unit — it is the proof of the technique, on the
   wall, at zero risk.
3. **Stage 2** uses that `do_spm` (now a normal callable routine) to rewrite
   pages 0–6. It writes a one-instruction trampoline into page 0 first, then
   pages 1–6 with read-back verify, then the real page 0 last. A reset or power
   loss between the trampoline and the final page-0 write boots the (still
   intact) application, which detects the half-done state and resumes.

## The interrupt-return trick (stage 1 only)

Writing page 7 needs an SPM, and no callable `do_spm` exists yet, so the app
borrows a boot-section SPM instruction and gets control back via an interrupt:

1. App sets `SPMCSR` for the operation (erase `0x03`, fill `0x01`, write
   `0x05`, rww-enable `0x11`), loads `Z` (and `r0:r1` for fill), and arms a
   timer compare interrupt to fire within a few cycles.
2. App jumps to a boot-section `spm` instruction.
3. The SPM halts the CPU for the NRWW page write/erase. When it completes the
   CPU resumes at the next (twiboot) instruction; the already-pending timer IRQ
   immediately vectors into the **application's** ISR (vectors are in app
   space), returning control to the app.

This works **only** for boot-section (NRWW) pages, where the CPU halts for the
write. For application-section (RWW) pages the CPU keeps running during the
write and would execute code it must not; that is why stage 2 does not use this
trick and instead calls the real `do_spm`.

The exact borrow site, `SPMCSR` values, timer setup and the entry/return
addresses are **locked by a test over the fielded hex** (see Verification) and
validated end-to-end in simavr before any unit is touched.

## New twiboot image (lean set)

Built from the vendored `main.c` + a small patch; delivered as a second
prebuilt hex plus a generated PROGMEM header for the unit firmware.

- **`do_spm` in page 7** at a fixed address, optiboot-style calling convention
  (`r24:r25` = address, `SPMCSR` action in a register, `r0:r1` = data word for
  fill), plus a 2-byte ABI/generation marker at `0x7FFE` so future firmware can
  identify the bootloader generation it runs under.
- **MCUSR stash:** `.init3` copies `MCUSR` into `GPIOR0` before clearing it, so
  the sketch can finally read the reset cause (today twiboot clears it — see
  #502 item 6, which this partly addresses).
- **Bounded pin:** a pinned bootloader falls back to the application after a
  quiet period, *unless* application flash word 0 reads `0xFFFF` (blank, keep
  waiting for a push). The quiet period is anchored to the measured worst-case
  reflash batch gap, not an invented number.
- **EEPROM access removed** (the master never uses twiboot's EEPROM path) to
  buy space. Measured budget with avr-gcc 7.3.0: EEPROM off = 728 B text,
  EEPROM+LED off = 694 B. Target image ≈ 790 B in pages 0–6 (896 B available),
  leaving headroom; `do_spm` lives in page 7.
- **Unchanged:** flash *write* stays bounded to the application section; flash
  *read* stays unbounded so `boot-dump` keeps working.

The generated PROGMEM header is gated to match the prebuilt hex, the same way
the unit bundle is gated against the Unit build.

## Unit firmware: report + updater

All additive to the I2C contract — no `SFP_PROTOCOL_VERSION` bump.

### `GET_BOOT_INFO` (query, 0x8B)

Checksummed reply carrying: lock byte, three fuse bytes, CRC32 of the boot
section, a classified boot-section state, and the last update result. Reading
lock/fuses is done by **application** code (twiboot cannot report them). This
closes **#502 item 8**.

Boot-section state is derived from flash, never stored — a pure,
natively-tested classifier in a new `shared/` header maps the section to:
`Old` (CRC == `18173add`), `Page7Installed`, `Trampoline`, `New`, or `Unknown`.

### `BOOT_UPDATE` (mutation, 0x9A, stage byte + complement)

Deferred to `loop()` via a `pending*` flag like every other mutation; refused
while the drum is moving or not homed.

- **Stage 1** allowed only from `Old` with lock bits permitting boot writes.
  Writes `do_spm` into page 7 via the interrupt-return trick, re-verifies page
  7 by read-back, records the result, and continues running. Brick-free.
- **Stage 2** allowed only from `Page7Installed` or `Trampoline`. Writes the
  page-0 trampoline (`jmp 0` to the application), then pages 1–6 with per-page
  read-back verify, then the real page 0, then verifies the whole-section CRC
  against the target and resets into the new bootloader.
- **Auto-resume:** a unit that boots and classifies as `Trampoline` runs stage
  2 once unprompted — it has no working bootloader until stage 2 finishes, so
  it must not wait for a command.

The updater and the embedded target image stay in the unit firmware afterwards
(~1.6 KB; unit firmware uses 14.2 KB of 30.7 KB today) as the routine path for
future bootloader updates.

## Master (S3)

Mirrors the stage-1 (#511) op structure: a `DisplayOpcode`, a
`makeBootUpdateCommand`, an `exec*` in `DisplayTask.cpp`, a `UnitBus` entry, and
two web routes.

- **`POST /unit/boot-update?address=N&stage=1|2`** and **`POST
  /unit/boot-info?address=N`** / result reads, on the existing `{"seq":N}`
  op-result contract.
- Arms the probe-inhibit deadline after stage 2 (as `/unit/reboot` and the
  address burns do).
- **Reflash-order change (load-bearing):** the normal unit reflash job writes
  application page 0 **last** (blank it first). An interrupted reflash then
  leaves word 0 blank, so both the old and the new twiboot stay in the
  bootloader for auto-install. This is what makes the new twiboot's bounded pin
  safe.
- **`flashing/update-unit-bootloaders.sh`:** gated, per-unit, stops at the
  first failure — the twiboot-generation twin of `commission-units.sh`.

## Fielding sequence

1. OTA the master, then an ordinary unit reflash campaign carrying the report +
   updater (no EEPROM-layout bump, so it is a cheap reflash).
2. `GET_BOOT_INFO` on all 16 units; confirm `Old` and lock bits open.
3. Stage 1 on all 16; confirm `Page7Installed` by read-back.
4. Stage 2 on **one** unit, then accept it three ways: `boot-dump` CRC through
   the new twiboot, a full application reflash through it, and the unit homing
   afterwards.
5. Stage 2 on the remaining 15, one at a time, stopping at the first failure.

## Verification

- **Native tests** (`pio test -e native`, Unit + Master): the boot-section
  classifier, the stage guards, and the reply encoders.
- **Entry-point gate** (pytest): asserts the fielded hex (CRC `18173add`) still
  carries the exact instruction bytes at every address the stage-1 borrow
  depends on, and the exact `do_spm` entry in the new image — proven by breaking
  it once (per the verify-guards-by-falsification rule). A toolchain or image
  change that moves a byte fails CI rather than bricking a unit.
- **Generated-header gate** (pytest): the PROGMEM target image header matches
  the prebuilt hex.
- **simavr** (installed on the build host), standing in for the declined
  spare-Nano bench stage: run the real updater against the real fielded image,
  including a kill mid-stage-2 and a resume, and diff the resulting boot section
  against the target. It does not model SPM timing or lock bits, but it
  exercises the register setup, addresses and page ordering — where a defect
  would actually live.
- **Review:** `cpp-reviewer` on the unit updater and the master reflash-order
  change (OTA/flash/concurrency surface).

## Risks and non-goals

- A power loss during either page-0 write (~9 ms each, twice per unit) bricks
  that unit until ICSP. This is the irreducible window; everything else is
  resumable.
- Lock bits are unknown until the new unit firmware reports them. A locked unit
  is refused and stays on the old twiboot.
- The ESP-01 row is out of scope for this arc (follow-up issue). Its units are
  still guarded by the boot-section CRC check even though they were never
  dumped.
- Not doing the "full #499 list": an in-twiboot application CRC check is
  deferred — it needs a length+CRC trailer and a wire change and does not fit
  the lean budget. The bounded pin + application-written page-0-last gives the
  interrupted-reflash safety without it.
