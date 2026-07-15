// main.cpp — ESP-01 cluster follower "dumb row" (#298, epic #270). One
// single-core superloop, no RTOS: async web handlers stage, loop() mutates
// (v1's context rule verbatim — the superloop plays both v2 roles: it
// drains staged work like netTask and owns the I2C bus like displayTask).
// Spec: docs/superpowers/specs/2026-07-14-v2-esp01-follower-design.md.
//
// Boot: I2C probe (post-twiboot window) + unit provisioning → WiFi (portal
// fallback) → SNTP + mDNS (plat=esp01 TXT) → endpoints. The row is blank
// until a leader joins it; a persisted membership boots into Grace.

#include <Arduino.h>
#include <ESP8266mDNS.h>
#include <ESPAsyncWebSrv.h>

#include "FollowerBus.h"
#include "FollowerCluster.h"
#include "FollowerConfig.h"
#include "FollowerWeb.h"
#include "FollowerWifi.h"

static AsyncWebServer webServer(80);

void setup() {
#if SERIAL_ENABLE == true
  Serial.begin(SERIAL_BAUDRATE);
#endif
  SerialPrintln(F(""));
  SerialPrintln(F("#######################################################"));
  SerialPrintln(F(".........Split Flap ESP-01 Follower Starting..........."));
  SerialPrintln(F("#######################################################"));

  busInit();
  clusterInit();  // EEPROM membership → Grace/Standalone

  // Early I2C scan — deliberately AFTER twiboot's ~1 s window (v1 #88:
  // probing inside it pins the bootloader alive), then provision any
  // blank-app units from the PROGMEM bundle.
  SerialPrintln(F("Early I2C scan (post-twiboot window)..."));
  delay(1500);
  busProbe();
  busAutoInstallBootloaderUnits();

  wifiInit(webServer);
  if (!isWifiConfigured) {
    // Portal saved credentials or timed out — loop() reboots us.
    return;
  }

  wifiServicesInit(displayWidth);

  // Settled pass (v1 flow): re-probe, catch stragglers, self-heal any unit
  // not on the bundled rev, then warm the health facts.
  busProbe();
  busAutoInstallBootloaderUnits();
  busAutoUpdateOutdatedUnits();
  busPollHealth();

  webEndpointsInit(webServer);
  delay(250);
  webServer.begin();

  SerialPrintln(F("ESP-01 follower ready — waiting for a leader"));
  SerialPrintln(F("#######################################################"));
}

void loop() {
  if (isPendingReboot) {
    SerialPrintln(F("Rebooting now..."));
    // Let AsyncWebServer flush the response before the restart yanks the
    // socket (v1 #37 value).
    delay(500);
    ESP.restart();
    return;
  }

  if (!isWifiConfigured) {
    delay(100);
    return;
  }

  MDNS.update();

  // Freeze all display/unit work while a master firmware image streams in
  // (v1 #116); the stalled-upload auto-thaw (incl. freeing the Update
  // session slot, v1 #191) lives with the session state in FollowerWeb.
  if (webOtaUploadFrozen()) {
    delay(50);
    return;
  }

  webLoopTick();      // staged ops / reflash / health refresh
  clusterLoopTick();  // phase decay, blanking, due renders
  followerDiagTick(); // fold current heap into the since-boot min (#306)

  delay(2);
}
