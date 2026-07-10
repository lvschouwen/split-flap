#pragma once

// UnitBus — the v2 master's I2C access to the v1 units (#203, slice A of
// the I2C port). The ONLY Wire toucher in the firmware, and displayTask is
// its only caller (#187 rule: bus exclusivity is structural — no busy
// flags, the command queue is the staging mechanism). Blocking by design:
// a straight port of v1's bench-proven transactions and timing; commands
// queue behind long operations.
//
// All functions here have hardware side effects and are bench-tested (the
// pure seams they lean on — FlapFrame, UnitHealth, DisplayWidth — are the
// natively-tested tier).

#include <stdint.h>

#include "UnitHealth.h"

// Wire init on the unit bus pins. SDA=8 / SCL=9 (Arduino-ESP32 S3 defaults),
// 100 kHz, 3.3 V — electrically a drop-in for the ESP-01. Clear of the
// reserved pins (4 button, 19/20 USB, 35-37 PSRAM, 48 LED).
void unitBusInit();

// Scans I2C addresses base..base+maxUnits-1 and fills facts[i].state
// (0 silent / 1 sketch / 2 bootloader), plus version + fwStatus for
// sketch-mode units. Resets every slot first — a re-probe forgets units
// that dropped off the bus.
void unitBusProbe(UnitFacts* facts, int maxUnits);

// Reads CMD_GET_STATUS from every sketch-mode unit into facts[i].status /
// statusValid. Bootloader/silent slots stay invalid so they render as gaps
// and never count toward the faulty total.
void unitBusPollHealth(UnitFacts* facts, int maxUnits);

// Drives one frame onto the flaps: waits for the display to stop, sends
// letters[0..width-1] to every sketch-mode unit, waits again, then verifies
// each unit via CMD_GET_LETTER with one resend round (v1 #106 closed loop).
// `unitSpeed` is the wire speed (MIN_SPEED..MAX_SPEED, already converted).
// Returns the number of failed unit writes (v1's writeErrors tally).
int unitBusShowFrame(const UnitFacts* facts, int width,
                     const uint8_t* letters, int unitSpeed);
