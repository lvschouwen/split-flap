// WiFi bring-up: STA connect with Preferences-backed credentials, AP
// fallback when no creds are saved or the connect attempt times out.
//
// Provisioning model: the user either presses the AP (SSID "SplitFlap-
// Setup") from a laptop/phone, browses to http://192.168.4.1/, fills in
// the SSID + password form, and reboots; or the device already has creds
// stored and comes up in STA mode talking to the home router.
//
// Device name (mDNS-ish label, exposed on /settings) is a Preferences
// string in the same namespace.

#pragma once

#include <Arduino.h>
#include <IPAddress.h>

namespace wifiSetup {

// Attempt STA connect with whatever is in Preferences. Falls back to AP
// mode if no creds are stored or the connect times out. Non-blocking
// beyond the connect timeout (~15 s worst case).
void begin();

bool      isStaConnected();
bool      isApMode();
IPAddress currentIP();          // STA IP if connected, else AP IP.
String    ssid();               // current SSID (STA or AP).
String    deviceName();         // from Preferences (fallback "splitflap-s3").

// Persist new creds and mark a pending reboot. The main loop should call
// applyPendingReboot() to reboot at a safe point (after HTTP response
// completes).
void saveCredentials(const String& ssid, const String& password);
void saveDeviceName(const String& name);

// Mark a reboot as pending; main loop should call applyPendingReboot() to
// actually reboot at a safe point (after the HTTP response has flushed).
void markRebootPending();
bool rebootPending();
void applyPendingReboot();

}  // namespace wifiSetup
