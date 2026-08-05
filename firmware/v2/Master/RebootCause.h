// Deliberate-reboot breadcrumb (#432): the WiFi watchdog and the web/MQTT
// reboot paths log their reason once to the flash-log tee, whose ring holds
// only a few hours — after that, the next boot's /settings says just
// "Software reset". These two calls make the cause durable: stamp it into
// NVS at the moment a deliberate restart is scheduled, consume it exactly
// once on the following boot, and let /settings serve it as lastRebootCause.
//
// Panic/watchdog resets never stamp (the panic path cannot write NVS and
// does not need to: lastResetReason already distinguishes them, and the
// coredump carries the detail), so an empty consume result means "this boot
// was not a stamped deliberate reboot".
//
// Context rule: both functions write NVS — call them from netTask (stamp
// sites: WifiService tick, webEndpointsLoop's reboot drain) or from
// single-threaded setup() (consume), never from an async handler.
#pragma once

#include <Arduino.h>

// Persist the human-readable cause for the restart about to happen.
void rebootCauseStamp(const String& cause);

// Read and clear the stamped cause; "" when the last reset was not a
// stamped deliberate reboot. The first call clears NVS and caches — call it
// at the TOP of setup(), before any init that can panic, so a stamp can
// never be misattributed to a later boot after a crash-looping init; later
// callers (webEndpointsInit) get the cached copy. Single-threaded until the
// cache is primed.
String rebootCauseConsume();
