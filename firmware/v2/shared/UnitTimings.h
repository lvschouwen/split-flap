#pragma once
// UnitTimings.h — the waits a row master owes a Nano unit, each named once
// with the reason it has that value. Both row masters drive the same units
// through the same bootloader, so none of these is a per-tree choice.

// After CMD_ENTER_BOOTLOADER before talking to twiboot: watchdog reset
// (~15 ms) + twiboot init. 500 ms is generous.
#define TWIBOOT_STARTUP_MS 500

// Before the first bus probe after power-up. Twiboot waits ~1 s for a command
// before starting the sketch, and a probe landing inside that window pins the
// bootloader alive (its CHIPINFO read zeroes the boot timeout).
#define UNIT_BOOT_PREPROBE_DELAY_MS 1500UL

// How long runtime probes stay off the bus after anything that resets a unit
// through its bootloader (reboot, address burn, boot update, boot dump): the
// ~1 s twiboot window plus the start of homing, with the same margin class as
// the pre-probe delay.
#define UNIT_PROBE_INHIBIT_MS 3000UL

// A restarted unit is back in its sketch and answering within this.
#define UNIT_RETURN_TIMEOUT_MS 10000UL
// One full homing revolution at homing speed, with margin.
#define UNIT_HOME_TIMEOUT_MS 20000UL

// A render waits for the row to stop before the next frame; a unit still
// reporting motion after this is physically stuck (status byte pegged at 1)
// and the wait moves on — a jammed flap is a mechanical condition the row
// master survives, not a reboot cause. Longer than a task watchdog timeout,
// so the poll loop feeds the watchdog itself (#314).
#define UNIT_SHOW_STUCK_TIMEOUT_MS 30000UL
