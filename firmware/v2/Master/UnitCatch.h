#pragma once
// UnitCatch.h — remembers one unit to catch in its bootloader at the next
// power-on (#554). Rules: UnitCatchPolicy.h. NVS namespace `sfboot`.

#include <stdint.h>

// First thing in setup(), before its start-up delay: when armed, asks the
// unit's address for its bootloader until one answers or the window is over.
// Returns how long that took, so the caller can shorten its delay.
uint32_t unitCatchAtPowerOn();

// Once the log is up: what unitCatchAtPowerOn() found.
void unitCatchLogReport();

// The armed address, 0 = none. Cached at start; arm/disarm write through.
uint8_t unitCatchArmed();
void unitCatchArm(uint8_t addr);
void unitCatchDisarm();
