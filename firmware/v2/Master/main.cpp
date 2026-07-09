// v2 master — Phase 1 scaffold (#184, meta #58, epic #183).
//
// Boots on an ESP32-S3 devkit, prints an identity banner, and keeps the
// ported pure-logic headers compiling under the ESP32 Arduino core (they
// are otherwise only exercised by the native test env).

#include <Arduino.h>
// SettingsEepromLayout.h uses the EEPROM global without including it (in v1
// the include comes from ESPMaster.ino's TU). The ESP32 core provides an
// EEPROM emulation over flash; whether v2 keeps the v1 blob layout on it or
// moves to NVS is an open Phase 1 decision — see #58.
#include <EEPROM.h>

#include "DeviceIdentity.h"
#include "DisplayWidth.h"
#include "MdnsDiscovery.h"
#include "MqttHelpers.h"
#include "SettingsEepromLayout.h"
#include "SettingsValidation.h"

// v1 derives its chip id from ESP.getChipId() = last 3 octets of the MAC.
// The ESP32 core has no getChipId(); take the same last-3-octets slice of
// the efuse base MAC so a device keeps one identity across the port.
// TODO(#58): lock byte-order parity against a v1 device before the
// identity/EEPROM migration lands.
static uint32_t chipIdFromEfuseMac() {
  const uint64_t mac = ESP.getEfuseMac();  // base MAC, byte 0 = first octet
  return (uint32_t)((mac >> 24) & 0xFFFFFF);
}

static const char* MDNS_NAME_PREFIX = "split-flap";

void setup() {
  Serial.begin(115200);
  delay(2000);  // native USB-CDC needs a moment before the first prints land

  const String deviceName = defaultDeviceName(MDNS_NAME_PREFIX, chipIdFromEfuseMac());
  Serial.println();
  Serial.println(F("split-flap v2 master — Phase 1 scaffold (#184)"));
  Serial.printf("chip: %s rev %d, %d cores @ %d MHz\n", ESP.getChipModel(),
                ESP.getChipRevision(), ESP.getChipCores(), ESP.getCpuFreqMHz());
  Serial.printf("flash: %u KB, free heap: %u KB\n",
                ESP.getFlashChipSize() / 1024, ESP.getFreeHeap() / 1024);
  Serial.printf("identity: %s\n", deviceName.c_str());
}

void loop() {
  static uint32_t lastTick = 0;
  if (millis() - lastTick >= 5000) {
    lastTick = millis();
    Serial.printf("[%8lu ms] heartbeat, free heap %u KB\n",
                  (unsigned long)millis(), ESP.getFreeHeap() / 1024);
  }
}
