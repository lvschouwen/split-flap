// Boot-stage trace glue (#504, policy in BootTracePolicy.h). One NVS blob in
// the "sfboot" namespace, rewritten each time this boot reaches a new stage,
// so the boot after an outage — however it ended — reports how far the
// failed boots got and what reset each of them.
#pragma once

#include <Arduino.h>

#include "BootTracePolicy.h"

// Top of setup(), single-threaded: loads the ring and appends this boot.
void bootTraceInit();

// Once the log is up (after flashLogInit()), still single-threaded: the
// previous boots, when any of them failed to come online.
void bootTraceLogReport();

// Any task: records that this boot reached `stage` (writes NVS only when the
// stage advances — at most once per stage per boot).
void bootTraceMarkStage(uint8_t stage);

// /settings "bootTrace": oldest first, this boot last.
String bootTraceJson();
