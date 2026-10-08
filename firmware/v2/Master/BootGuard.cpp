#include "BootGuard.h"

#include <Preferences.h>
#include <esp_attr.h>
#include <esp_system.h>

#include <atomic>

#include "BuildVersion.h"
#include "FactorySlot.h"
#include "HelpersSerialHandling.h"
#include "SettingsJson.h"  // appendJsonString
#include "WebEndpoints.h"  // webResetReasonName

static const char* kNamespace = "sfboot";  // shared with RebootCause.cpp
static const char* kTripsKey = "gTrips";
static const char* kReasonKey = "gReason";
static const char* kRevKey = "gRev";

RTC_NOINIT_ATTR static BootGuardRecord guardRecord;

// Read by the web task; the record itself is written by setup() and netTask
// only.
static std::atomic<uint8_t> crashesNow{0};

// Loaded once in bootGuardBoot(), before any task exists; never written
// after, so the web task reads them without a lock.
static uint32_t trips = 0;
static uint8_t tripReason = 0;
static String tripRev;

static void loadTrip() {
  Preferences prefs;
  if (!prefs.begin(kNamespace, /*readOnly=*/true)) return;
  trips = prefs.getUInt(kTripsKey, 0);
  tripReason = prefs.getUChar(kReasonKey, 0);
  tripRev = prefs.getString(kRevKey, "");
  prefs.end();
}

static void saveTrip(uint8_t reason) {
  Preferences prefs;
  if (!prefs.begin(kNamespace, /*readOnly=*/false)) {
    SerialPrintln(F("boot guard: NVS open failed — trip not noted"));
    return;
  }
  prefs.putUInt(kTripsKey, trips + 1);
  prefs.putUChar(kReasonKey, reason);
  prefs.putString(kRevKey, GIT_REV);
  prefs.end();
}

void bootGuardBoot() {
  const int reason = (int)esp_reset_reason();
  const uint8_t crashes = bootGuardStep(bootGuardDecode(guardRecord), reason);
  bootGuardEncode(guardRecord, crashes);
  crashesNow.store(crashes, std::memory_order_relaxed);
  loadTrip();
  if (!bootGuardShouldTrip(crashes)) return;

  SerialPrintf("boot guard: %u crashes in a row (last: %s)\n", (unsigned)crashes,
               webResetReasonName(reason));
  // A slot that would not start is no way out: the bootloader would come
  // straight back to this image, with otadata gone.
  if (!factorySlotImageVerified()) {
    SerialPrintln(F("boot guard: no rescue image that verifies — staying on "
                    "this image"));
    return;
  }
  if (!rescueBootArm()) return;
  // After the erase: a trip is only noted once the rescue image is what
  // starts next.
  saveTrip((uint8_t)reason);
  bootGuardEncode(guardRecord, 0);
  SerialPrintln(F("boot guard: restarting into the rescue image"));
  Serial.flush();
  esp_restart();
}

void bootGuardTick() {
  static bool forgiven = false;
  if (forgiven || !bootGuardHealthy(millis())) return;
  forgiven = true;
  bootGuardEncode(guardRecord, 0);
  crashesNow.store(0, std::memory_order_relaxed);
}

String bootGuardJson() {
  String out = "{\"crashes\":";
  out += String((unsigned)crashesNow.load(std::memory_order_relaxed));
  out += ",\"limit\":";
  out += String(BOOT_GUARD_CRASH_LIMIT);
  out += ",\"trips\":";
  out += String((unsigned long)trips);
  if (trips > 0) {
    out += ",\"lastTrip\":{\"reset\":";
    appendJsonString(out, webResetReasonName(tripReason));
    out += ",\"rev\":";
    appendJsonString(out, tripRev);
    out += '}';
  }
  out += '}';
  return out;
}
