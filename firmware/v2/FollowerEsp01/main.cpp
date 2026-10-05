// main.cpp — ESP-01 row board (#298; wall link #559). One single-core
// superloop, no RTOS: web handlers stage, loop() mutates — it reads the wall
// link, runs unit jobs and owns the I2C bus.
// Spec: docs/superpowers/specs/2026-10-05-wall-link-and-console-design.md.
//
// Boot: I2C probe (post-twiboot window) + unit provisioning → WiFi (portal
// fallback) → SNTP + mDNS (plat=esp01 TXT) → routes. The row is blank until
// a master pairs it; a stored pairing boots into Grace and dials the master.

#include <Arduino.h>
#include <ESP8266mDNS.h>
#include <ESPAsyncWebServer.h>

#include "UnitTimings.h"
#include "FollowerEscalation.h"
#include "FollowerLink.h"
#include "FollowerBus.h"
#include "FollowerCluster.h"
#include "FollowerConfig.h"
#include "FollowerPrefs.h"
#include "FollowerRescue.h"
#include "FollowerResetLog.h"
#include "FollowerUnitJobs.h"
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
  resetLogBootInit();  // #503: why we restarted, kept across soft resets
  escalationBootInit();  // #503: self-restarts taken, for the rate limit

  busInit();
  clusterInit();  // stored pairing → Grace, none → Standalone
  prefsInit();    // #513: needs clusterInit()'s EEPROM.begin
  if (!prefsReflashOnBoot()) {
    // Deliberately suppressed for a gated campaign. Say so loudly — a skipped
    // auto-install looks exactly like a healthy row, and this setting
    // persists across reboots.
    SerialPrintln(F("reflash: boot auto-install SUPPRESSED (reflashOnBoot=false)"));
  }

  if (!rescueActive()) {
#ifdef RESCUE_CRASH_TEST
    // #343 bench-drill hook (build with -DRESCUE_CRASH_TEST; never a real
    // build): simulates a poisoned image dying in a path rescue mode skips —
    // 3 fast crash cycles, then rescue mode engages and the master offers
    // its stored image.
    SerialPrintln(F("RESCUE_CRASH_TEST: crashing this boot on purpose"));
    delay(100);
    abort();
#endif
    // Early I2C scan — deliberately AFTER twiboot's ~1 s window (v1 #88:
    // probing inside it pins the bootloader alive), then provision any
    // blank-app units from the PROGMEM bundle.
    SerialPrintln(F("Early I2C scan (post-twiboot window)..."));
    delay(UNIT_BOOT_PREPROBE_DELAY_MS);
    busProbe();
    if (prefsReflashOnBoot()) busAutoInstallBootloaderUnits();
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
    if (prefsReflashOnBoot()) {
      busAutoInstallBootloaderUnits();
      busAutoUpdateOutdatedUnits();
    }
    busPollHealth();
  }

  webEndpointsInit(webServer);
  delay(250);
  webServer.begin();

  if (!rescueActive()) {
    // Staggered boot-home (#309): the units boot UNHOMED, so home the row in
    // bounded batches instead of letting the master's first text home the whole
    // row at once (the #305 power-up brownout class). Run AFTER webServer.begin()
    // so the upload route answers from the SDK/LWIP context during
    // followerBootHome()'s delay()s — this is a single-core board, so a slow
    // homing sweep (bad halls) would otherwise leave it unreachable.
    followerBootHome();
    // Boot sections after the boot-home: stage 2 needs a homed unit, and one
    // homed here is not homed again for it (UnitUpdateJob.h). Same brake as
    // the application auto-update.
    if (prefsReflashOnBoot()) busAutoUpdateBootSections();
  }

  SerialPrintln(rescueActive()
                    ? F("ESP-01 row in RESCUE MODE — WiFi, link and upload only")
                    : F("ESP-01 row ready — waiting for its master"));
  SerialPrintln(F("#######################################################"));
}

void loop() {
  if (isPendingReboot) {
    SerialPrintln(F("Rebooting now..."));
    // Deliberate restart (the master's Restart or a stored firmware image):
    // zero the bad-boot tally so the next image gets fresh chances — this is
    // also rescue mode's one exit (#343).
    rescueMarkHealthy();
    // A setting accepted moments ago must not die with this restart (#513).
    prefsLoopTick(true);
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

  // Freeze all display/unit work while a firmware upload streams in
  // (v1 #116); the stalled-upload auto-thaw (incl. freeing the Update
  // session slot, v1 #191) lives with the session state in FollowerWeb.
  if (webOtaUploadFrozen()) {
    delay(50);
    return;
  }

  followerTxTick();     // #508: WiFi TX power ladder, 1 Hz
  prefsLoopTick();      // #513: persist a changed setting
  rescueHealthyTick();  // #343: a stable minute proves this boot good
  if (!rescueActive()) {
    unitJobsLoopTick();       // the staged unit job, self-test poll, rescan
    followerHeartbeatTick();  // one scheduled unit-health read per tick (#310)
  }
  // The wall link to the master. Runs in rescue mode too: the master sees
  // the rescue flag in Hello and offers the image that ends it.
  linkLoopTick();
  clusterLoopTick();  // phase decay, blanking, due renders (bus-gated in rescue)
  followerDiagTick(); // fold current heap into the since-boot min (#306)
  escalationTick();   // #503: a fault that did not heal ends in a restart

  delay(2);
}
