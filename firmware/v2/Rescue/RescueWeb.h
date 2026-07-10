#pragma once
// RescueWeb — the rescue app's entire web surface (#195): the self-contained
// page, GET /rescue/status (slot inventory), POST /firmware/master (same
// wire contract as normal firmware: multipart + mandatory ?md5=, target
// app0), and POST /rescue/exit (boot the newest valid OTA slot, no flash
// write beyond otadata). Decision logic lives in the natively-tested
// RescueOta.h / RescueSlots.h; this TU is target glue.

#include <Arduino.h>
#include <ESPAsyncWebServer.h>

// setup() context: registers all routes. Server start is separate — LWIP
// isn't up before a netif exists.
void rescueWebInit(AsyncWebServer& server, const String& deviceName);

// Idempotent server.begin(); called by whichever netif comes up (AP or STA).
void rescueWebStart(AsyncWebServer& server);

// Captive-portal hook: while the -rescue AP is up, onNotFound redirects
// every hostname here ("" = no redirect, plain 404).
void rescueWebSetCaptiveRedirect(const String& url);

// loop() context: executes a staged reboot after a grace period so the
// HTTP response that triggered it flushes first.
void rescueWebTick();
