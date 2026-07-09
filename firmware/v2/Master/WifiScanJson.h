#pragma once
// WifiScanJson.h — pure JSON builder for GET /wifi/scan (#188), the portal
// page's scan-and-pick list. Raw WiFi.scanNetworks() results go in; out
// comes one row per network: deduped by ssid at the strongest RSSI (mesh /
// repeater setups scan once per BSSID), strongest first, hidden (empty-ssid)
// APs dropped, capped so the String stays bounded in a dense environment.
// Natively tested (test/test_wifi_scan_json); WifiService.cpp only adapts
// the scan results into WifiScanEntry[].

#include <Arduino.h>

#include "SettingsJson.h"  // appendJsonString escaper

#define WIFI_SCAN_JSON_MAX 20

struct WifiScanEntry {
  String ssid;
  int32_t rssi = -127;
  bool secure = true;
};

inline String buildWifiScanJson(const WifiScanEntry* entries, size_t count) {
  // Dedup + selection-sort into a fixed-size strongest-first list. count is
  // a scan result (tens), so O(n^2) beats dragging in a heap allocation.
  const WifiScanEntry* best[WIFI_SCAN_JSON_MAX];
  size_t kept = 0;

  for (size_t i = 0; i < count; i++) {
    const WifiScanEntry& e = entries[i];
    if (e.ssid.length() == 0) continue;  // hidden AP — manual entry covers it

    // Same ssid already kept? Keep whichever is stronger — and bubble the
    // replacement up, since a stronger reading can outrank earlier rows.
    bool duplicate = false;
    for (size_t k = 0; k < kept; k++) {
      if (best[k]->ssid == e.ssid) {
        if (e.rssi > best[k]->rssi) {
          best[k] = &e;
          while (k > 0 && best[k - 1]->rssi < best[k]->rssi) {
            const WifiScanEntry* tmp = best[k - 1];
            best[k - 1] = best[k];
            best[k] = tmp;
            k--;
          }
        }
        duplicate = true;
        break;
      }
    }
    if (duplicate) continue;

    // Insert sorted by descending rssi.
    size_t at = kept;
    while (at > 0 && best[at - 1]->rssi < e.rssi) at--;
    if (at >= WIFI_SCAN_JSON_MAX) continue;  // weaker than the full list
    if (kept < WIFI_SCAN_JSON_MAX) kept++;
    for (size_t j = kept - 1; j > at; j--) best[j] = best[j - 1];
    best[at] = &e;
  }

  String out;
  out.reserve(24 + kept * 48);
  out += "{\"networks\":[";
  for (size_t k = 0; k < kept; k++) {
    if (k > 0) out += ',';
    out += "{\"ssid\":";
    appendJsonString(out, best[k]->ssid);
    out += ",\"rssi\":";
    out += String(best[k]->rssi);
    out += ",\"secure\":";
    out += best[k]->secure ? "true" : "false";
    out += '}';
  }
  out += "]}";
  return out;
}
