#pragma once
// FollowerWifi.h — WiFi bring-up + identity + SNTP + mDNS advertise
// (#298): v1's proven flow trimmed. Credentials live in exactly ONE place,
// the ESP8266 SDK's flash config sector — the captive setup portal
// ("<name>-setup") writes them, a bare WiFi.begin() reads them back.
// Static IPs unsupported (DHCP reservation, v1 rule).

#include <Arduino.h>
#include <ESPAsyncWebSrv.h>

// "split-flap-<hex chip id>" — no rename on this firmware.
extern String effectiveDeviceName;
extern bool isWifiConfigured;

// Resolves the identity, joins the stored WiFi (30 s) or opens the setup
// portal (may set the pending-reboot flag after a portal save/timeout).
void wifiInit(AsyncWebServer& server);

// SNTP (epoch only — commitAt flip sync needs no timezone) + the
// _splitflap._tcp advertisement (TXT name/rev/width/plat=esp01, #297) so
// the leader's Cluster-card scan finds this row.
void wifiServicesInit(int rowWidth);
