#include "OtaService.h"

#include <Preferences.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "BuildVersion.h"
#include "FactorySlot.h"
#include "HelpersSerialHandling.h"
#include "SlotRecord.h"

// Snapshotted once in otaServiceInit() (single-threaded setup); `confirmed`
// is the only field that changes afterwards (netTask writes, async /settings
// handler reads — hence the mutex).
static bool pendingAtBoot = false;
static bool rolledBack = false;
static bool confirmed = false;
static bool confirmInFlight = false;  // claim guard between check and otadata write
static SemaphoreHandle_t otaStateMutex = nullptr;

struct OtaStateLock {
  OtaStateLock() { xSemaphoreTake(otaStateMutex, portMAX_DELAY); }
  ~OtaStateLock() { xSemaphoreGive(otaStateMutex); }
  OtaStateLock(const OtaStateLock&) = delete;
  OtaStateLock& operator=(const OtaStateLock&) = delete;
};

// Strong override of the core's weak hook (esp32-hal-misc.c, C linkage):
// returning true skips initArduino()'s auto-confirm so a broken image that
// crashes before the netif comes up is rolled back by the bootloader.
extern "C" bool verifyRollbackLater() { return true; }

void otaServiceInit() {
  otaStateMutex = xSemaphoreCreateMutex();
  if (otaStateMutex == nullptr) {
    Serial.println(F("FATAL: otaStateMutex allocation failed"));
    abort();  // panic into the coredump partition, same as the other inits
  }

  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_ota_img_states_t state;
  if (esp_ota_get_state_partition(running, &state) == ESP_OK) {
    pendingAtBoot = (state == ESP_OTA_IMG_PENDING_VERIFY);
  }
  rolledBack = esp_ota_get_last_invalid_partition() != nullptr;

  if (pendingAtBoot) {
    Serial.printf("ota: image on %s is PENDING_VERIFY — confirm armed pre-inrush\n",
                  running->label);
  } else if (rolledBack) {
    Serial.println(F("ota: bootloader ROLLED BACK a failed image — running the previous slot"));
  }
}

// #200: /rescue/exit needs an ordering signal that survives both the
// otadata erase on rescue entry and the app-descriptor build stamp being
// frozen at framework-assembly time under pioarduino hybrid builds. The
// running slot gets a confirm record in NVS (shared "splitflap" namespace,
// keys slotRec0/slotRec1): monotonic seq + image sha256 + GIT_REV. Rescue
// ranks exit candidates by seq after checking sha against the slot's actual
// image; the record format is wire-contract-like (SlotRecord.h /
// RescueSlotRecord.h) because Rescue parses it from a separately compiled
// image. sha comes from the same esp_partition_get_sha256 both sides use,
// so a slot reflashed behind the record's back demotes itself to the
// stamp fallback instead of inheriting a stale seq.
static void ensureSlotRecord() {
  const esp_partition_t* running = esp_ota_get_running_partition();
  int slot;
  if (running != nullptr && running->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0) {
    slot = 0;
  } else if (running != nullptr &&
             running->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1) {
    slot = 1;
  } else {
    return;  // not an OTA slot — nothing for rescue to rank
  }

  // Hashes the actual image length (not the 4 MB partition) — IDF verifies
  // the appended digest over image_len. Once per boot; the ms print is the
  // bench check that this stays cheap on netTask.
  uint8_t sha[32];
  uint32_t shaStart = millis();
  if (esp_partition_get_sha256(running, sha) != ESP_OK) {
    SerialPrintln(F("slot record: sha256 of running image failed"));
    return;
  }
  uint32_t shaMs = millis() - shaStart;

  Preferences prefs;
  if (!prefs.begin("splitflap", false)) {
    SerialPrintln(F("slot record: NVS open failed"));
    return;
  }
  SlotRecord rec0 = parseSlotRecord(prefs.getString("slotRec0", "").c_str());
  SlotRecord rec1 = parseSlotRecord(prefs.getString("slotRec1", "").c_str());
  const SlotRecord& mine = (slot == 0) ? rec0 : rec1;
  if (slotRecordShaMatches(mine, sha)) {
    prefs.end();
    // This exact image already holds the slot's record.
    SerialPrintln("slot record: app" + String(slot) + " current, seq " +
                  String(mine.seq) + " (sha " + String(shaMs) + " ms)");
    return;
  }

  uint32_t seq = nextSlotRecordSeq(rec0, rec1);
  char buf[SLOT_RECORD_BUF_LEN];
  if (formatSlotRecord(buf, sizeof(buf), seq, sha, GIT_REV)) {
    prefs.putString(slot == 0 ? "slotRec0" : "slotRec1", buf);
    SerialPrintln("slot record: app" + String(slot) + " = " GIT_REV " seq " +
                  String(seq) + " (sha " + String(shaMs) + " ms)");
  }
  prefs.end();
}

void otaHealthConfirm() {
  bool doConfirm = false;
  {
    OtaStateLock lock;
    // Atomically CLAIM the confirm with an in-flight flag so a would-be second
    // caller is a no-op instead of racing a concurrent otadata write. Callers
    // are non-overlapping today (setup() finishes before netTask exists), so
    // this only guards a future one — fail safe, not silently double-write.
    // `confirmed` itself is latched only once the write succeeds (below), so a
    // failed pre-inrush write still lets the netif-up fallback retry.
    if (pendingAtBoot && !confirmed && !confirmInFlight) {
      confirmInFlight = true;
      doConfirm = true;
    }
  }
  if (doConfirm) {
    // Flash write (otadata) outside the lock: /settings readers must not
    // stall behind it.
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    {
      OtaStateLock lock;
      confirmInFlight = false;
      if (err == ESP_OK) confirmed = true;
    }
    if (err == ESP_OK) {
      SerialPrintln(F("OTA image confirmed (pre-inrush) — rollback cancelled"));
    } else {
      SerialPrintln("OTA confirm FAILED: " + String(esp_err_to_name(err)));
      return;  // leave pending; the netif-up fallback call retries
    }
  }
  // Once per boot, on the first caller (setup pre-inrush, then netif-up):
  // also runs on non-pending boots so a pre-#200 device backfills its record
  // on the next power-up. Callers are temporally separated (setup finishes
  // before netTask exists), so the plain static needs no lock.
  static bool recordEnsured = false;
  if (!recordEnsured) {
    recordEnsured = true;
    ensureSlotRecord();
  }
}

OtaVerdict otaVerdictSnapshot() {
  bool pendingNow, confirmedNow, rolledBackNow;
  {
    OtaStateLock lock;
    pendingNow = pendingAtBoot && !confirmed;
    confirmedNow = confirmed;
    rolledBackNow = rolledBack;
  }
  return synthesizeOtaVerdict(rolledBackNow, pendingNow, confirmedNow);
}

String otaDebugJson() {
  const esp_partition_t* running = esp_ota_get_running_partition();
  const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
  const esp_partition_t* invalid = esp_ota_get_last_invalid_partition();
  OtaVerdict v = otaVerdictSnapshot();

  String out;
  out.reserve(192);
  out += "{\"running\":\"";
  out += running ? running->label : "?";
  out += "\",\"next\":\"";
  out += next ? next->label : "?";
  out += "\",\"lastInvalid\":";
  if (invalid) {
    out += '"';
    out += invalid->label;
    out += '"';
  } else {
    out += "null";
  }
  out += ",\"lastFlashResult\":\"";
  out += v.lastFlashResult;  // fixed vocabulary, no escaping needed
  out += "\",\"otaReverted\":";
  out += v.otaReverted ? "true" : "false";
  out += ",\"factoryValid\":";  // rescue image installed? (#195 bench aid)
  out += factorySlotImageValid() ? "true" : "false";
  out += '}';
  return out;
}
