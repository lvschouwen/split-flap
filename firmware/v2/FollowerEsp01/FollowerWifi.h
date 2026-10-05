#pragma once
// FollowerWifi.h — WiFi bring-up + identity + SNTP + mDNS advertise
// (#298): v1's proven flow trimmed. Credentials live in exactly ONE place,
// the ESP8266 SDK's flash config sector — the captive setup portal
// ("<name>-setup") writes them, a bare WiFi.begin() reads them back.
// Static IPs unsupported (DHCP reservation, v1 rule).

#include <Arduino.h>
#include <ESPAsyncWebServer.h>

// "split-flap-<hex chip id>" — no rename on this firmware.
extern String effectiveDeviceName;
extern bool isWifiConfigured;

// Resolves the identity, joins the stored WiFi (30 s) or opens the setup
// portal (may set the pending-reboot flag after a portal save/timeout).
void wifiInit(AsyncWebServer& server);

// SNTP (epoch only — flip instants need no timezone) + the _splitflap._tcp
// advertisement (TXT name/rev/width/plat=esp01, #297) so a master's scan
// for boards to pair finds this row.
void wifiServicesInit(int rowWidth);

// #508: the WiFi TX power ladder (shared WifiTxPolicy.h) — every boot starts
// at the lowest level and moves one level at a time. The join and the portal
// step it inside wifiInit(); loop() calls the tick for the online half.
void followerTxTick();

// The level the radio was last set to, tenths of a dBm (0 before the first).
int followerTxPowerDbm10();

// Firmware upload guard (v1 #60 sag guard): while on, the ladder is frozen
// and the level is capped at the join ceiling; off restores the ladder level.
// Quiet (no log) — it is called from the async upload handler.
void followerTxOtaCap(bool on);

// #505: radio in a high-draw phase — an OTA writing, or the first minute of
// a reconnect. Unit motion holds until it settles (MotionBudget.h). A link
// down for longer is not "busy": the row must still show its clock fallback.
bool followerRadioBusy();
