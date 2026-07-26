#pragma once
// FactorySlot — Master-side flash operations for the rescue companions
// (#195): stream a rescue image into the `factory` partition
// (POST /firmware/rescue) and arm a rescue boot by erasing otadata
// (POST /firmware/rescue-boot). The factory slot is not an OTA slot, so the
// Update/esp_ota path can't target it — writes go through the raw
// esp_partition API. Installing never touches otadata: what boots next only
// changes via rescueBootArm().

#include <Arduino.h>

#include "RescueSlotStatus.h"

// The partition exists in the table (always true on the #193 layout; guards
// a mismatched table loudly instead of writing into nothing).
bool factorySlotPresent();

// What the rescue slot actually holds (#391) — partition truth, not a
// record of what someone meant to upload. Cached: the first call pays a
// ~60 ms sha256, every later one is free. Call once at boot (the banner
// does) so no web request ever triggers the computation.
RescueSlotFacts rescueSlotCurrent();

// Stamp the installed image's identity after a successful /firmware/rescue
// (NVS key slotRecF, SlotRecord.h format: sha256 binds the record to the
// image, so a slot rewritten out of band demotes itself to UNIDENTIFIED).
// An empty rev records the sha only — the slot then reads UNIDENTIFIED,
// which is honest. Refreshes the cache in place.
void rescueSlotRecordInstall(const String& rev);

// The slot holds a bootable-looking app image (validated app descriptor).
// Gate for /firmware/rescue-boot's 409: never strand a wall-mounted device
// pointed at an empty slot. Trustworthy even mid-upload: the install erases
// the header sector first and only writes the new header after the MD5
// verdict (FactoryChunkPlan.h), so a torn or in-flight image reads invalid
// here — in flash, across reboots.
bool factorySlotImageValid();

// An install started and hasn't reached a determinate outcome yet — and its
// stream showed activity within the last 30 s (a dropped connection never
// delivers a terminal callback; without the expiry it would wedge both
// rescue endpoints forever). Gates /firmware/rescue-boot (covers the window
// before the install's first erase lands) and a second /firmware/rescue
// (which must 409, not clobber the in-flight install's state).
bool factoryInstallInProgress();

// Streaming install, async_tcp-task context (same deliberate exception as
// Update.write in the /firmware/master handler — a firmware stream cannot
// be staged through a queue). begin resets state and validates the digest
// format upstream; chunks erase lazily and write, with flash sector 0 held
// back in RAM; end verifies the MD5, commits the header sector, and checks
// the resulting app descriptor. Failures after the slot was touched leave
// the header sector erased, so a torn upload can never be booted; failures
// before it (bad magic, oversize first chunk) leave a previous image intact.
bool factoryWriteBegin(const String& expectedMd5);
// streamOffset = the request's byte offset for this chunk (the framework's
// upload `index`); a mismatch with the install's own cursor means a foreign
// or out-of-order stream and fails the install instead of corrupting it.
bool factoryWriteChunk(const uint8_t* data, size_t len, size_t streamOffset);
bool factoryWriteEnd();
// "" while the last begin/chunk/end sequence has no error.
String factoryWriteError();

// Erase both otadata sectors: the next boot falls through to the factory
// slot. Flash-erase in handler context is the same accepted exception class
// as the streaming writes above (otadata is 8 KB — two sectors).
bool rescueBootArm();
