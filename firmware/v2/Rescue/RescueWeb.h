#pragma once
// RescueWeb — the rescue app's entire web surface (#195): the self-contained
// page, GET /rescue/status (slot inventory), POST /firmware/master (same
// wire contract as normal firmware: multipart + mandatory ?md5=, target
// app0), and POST /rescue/exit (boot the most recently confirmed valid OTA
// slot per Master's #200 NVS records, no flash write beyond otadata).
// Decision logic lives in the natively-tested RescueOta.h / RescueSlots.h /
// RescueSlotRecord.h; this TU is target glue.

#include <Arduino.h>
#include <ESPAsyncWebServer.h>

// setup() context: registers all routes and pins the #200 slot ranking
// (slotRec0/slotRec1 = Master's confirm records as read from NVS; sha
// verification against each slot image happens here, once). Server start is
// separate — LWIP isn't up before a netif exists.
void rescueWebInit(AsyncWebServer& server, const String& deviceName,
                   const String& slotRec0, const String& slotRec1);

// Idempotent server.begin(); called by whichever netif comes up (AP or STA).
void rescueWebStart(AsyncWebServer& server);

// Captive-portal hook: while the -rescue AP is up, onNotFound redirects
// every hostname here ("" = no redirect, plain 404).
void rescueWebSetCaptiveRedirect(const String& url);

// loop() context: executes a staged reboot after a grace period so the
// HTTP response that triggered it flushes first.
void rescueWebTick();
