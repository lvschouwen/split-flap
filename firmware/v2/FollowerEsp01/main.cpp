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
#include "FollowerRescue.h"
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

  // Boot-rescue tally (#343) FIRST — everything after this line is what a
  // crash-looping image never reaches.
  rescueBootInit();

  busInit();
  clusterInit();  // EEPROM membership → Grace/Standalone

  if (!rescueActive()) {
#ifdef RESCUE_CRASH_TEST
    // #343 bench-drill hook (build with -DRESCUE_CRASH_TEST; never a real
    // build): simulates a poisoned image dying in a beacon-skipped path —
    // 3 fast crash cycles, then the beacon engages and the leader
    // re-pushes the stored image.
    SerialPrintln(F("RESCUE_CRASH_TEST: crashing this boot on purpose"));
    delay(100);
    abort();
#endif
    // Early I2C scan — deliberately AFTER twiboot's ~1 s window (v1 #88:
    // probing inside it pins the bootloader alive), then provision any
    // blank-app units from the PROGMEM bundle.
    SerialPrintln(F("Early I2C scan (post-twiboot window)..."));
    delay(1500);
    busProbe();
    busAutoInstallBootloaderUnits();
  }

  wifiInit(webServer);
  if (!isWifiConfigured) {
    // Portal saved credentials or timed out — loop() reboots us.
    return;
  }

  wifiServicesInit(displayWidth);

  if (!rescueActive()) {
    // Settled pass (v1 flow): re-probe, catch stragglers, self-heal any unit
    // not on the bundled rev, then warm the health facts.
    busProbe();
    busAutoInstallBootloaderUnits();
    busAutoUpdateOutdatedUnits();
    busPollHealth();
  }

  webEndpointsInit(webServer);
  delay(250);
  webServer.begin();

  if (!rescueActive()) {
    // Staggered boot-home (#309): the units boot UNHOMED, so home the row in
    // bounded batches instead of letting the leader's first render home the whole
    // row at once (the #305 power-up brownout class). Run AFTER webServer.begin()
    // so the ESPAsync stack can answer /cluster/{join,ping} and /settings from
    // the SDK/LWIP context during followerBootHome()'s delay()s — this is a
    // single-core board, so a slow homing sweep (bad halls) would otherwise leave
    // it unreachable. Staged renders that arrive meanwhile wait for loop().
    followerBootHome();
  }

  SerialPrintln(rescueActive()
                    ? F("ESP-01 follower in RESCUE BEACON — OTA/cluster wire only")
                    : F("ESP-01 follower ready — waiting for a leader"));
  SerialPrintln(F("#######################################################"));
}

void loop() {
  if (isPendingReboot) {
    SerialPrintln(F("Rebooting now..."));
    // Deliberate restart (operator /reboot or a completed firmware upload):
    // zero the bad-boot tally so the next image gets fresh chances — this is
    // also the rescue beacon's one exit (#343).
    rescueMarkHealthy();
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

  rescueHealthyTick();  // #343: a stable minute proves this boot good
  if (!rescueActive()) {
    webLoopTick();            // staged ops / reflash / health refresh
    followerHeartbeatTick();  // one scheduled unit-health read per tick (#310)
  }
  clusterLoopTick();  // phase decay, blanking, due renders (bus-gated in rescue)
  followerDiagTick(); // fold current heap into the since-boot min (#306)

  delay(2);
}
