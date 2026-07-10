#include "FactorySlot.h"

#include <MD5Builder.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>

#include <cstring>

#include "FactoryChunkPlan.h"
#include "HelpersSerialHandling.h"

// Upload state. Only the async_tcp task touches it (uploads are serialized
// by the TCP stream, same rationale as WebEndpoints' otaRejection state).
static const esp_partition_t* target = nullptr;
static size_t writeOffset = 0;
static size_t erasedUpTo = 0;
static MD5Builder md5;
static String expectedMd5;
static String lastError;

// True from factoryWriteBegin() until the install reaches a determinate
// outcome (committed, failed-and-invalid, or rejected-untouched). Blocks
// rescueBootArm() for the window before the header-sector erase lands,
// gives /firmware/rescue-boot an honest 409 while an upload streams, and
// keeps a second upload from clobbering the in-flight one's state. A
// dropped connection never delivers a terminal callback, so the flag
// expires after a chunk-silence window instead of wedging both endpoints
// forever — the already-erased header sector keeps the flash-validity gate
// honest for that image regardless.
static bool installInProgress = false;
static uint32_t lastChunkMs = 0;
static const uint32_t INSTALL_STALE_MS = 30000;

// Flash sector 0 is held back here until factoryWriteEnd() verifies the MD5
// (see FactoryChunkPlan.h). Until the commit, the slot reads as invalid in
// flash itself — a torn or abandoned upload stays unbootable across reboots,
// not just while this boot's RAM state remembers it.
static const size_t SECTOR = 4096;
static uint8_t headerSector[SECTOR];

static const esp_partition_t* findFactory() {
  return esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                  ESP_PARTITION_SUBTYPE_APP_FACTORY, nullptr);
}

bool factorySlotPresent() { return findFactory() != nullptr; }

bool factorySlotImageValid() {
  const esp_partition_t* part = findFactory();
  if (part == nullptr) return false;
  esp_app_desc_t desc;
  return esp_ota_get_partition_description(part, &desc) == ESP_OK;
}

bool factoryInstallInProgress() {
  return installInProgress && (millis() - lastChunkMs) < INSTALL_STALE_MS;
}

static bool fail(const String& why) {
  lastError = why;
  SerialPrintln("Rescue install failed: " + why);
  // Only scrub flash the install actually touched: a failure before the
  // first erase must leave a previous valid image intact. After the header
  // commit (descriptor check failed) this erases the just-written header so
  // the slot reads cleanly invalid instead of holding a broken image.
  if (target != nullptr && erasedUpTo > 0) {
    esp_partition_erase_range(target, 0, SECTOR);
  }
  target = nullptr;
  installInProgress = false;
  return false;
}

bool factoryWriteBegin(const String& md5Param) {
  // Guard before touching any state: a second upload must not reclaim the
  // in-flight one's cursor/digest (the caller turns this into a 409).
  if (factoryInstallInProgress()) {
    lastError = "another rescue install is in flight";
    return false;
  }
  lastError = "";
  target = findFactory();
  if (target == nullptr) {
    lastError = "no factory partition in the table";
    installInProgress = false;
    return false;
  }
  writeOffset = 0;
  erasedUpTo = 0;
  expectedMd5 = md5Param;
  installInProgress = true;
  lastChunkMs = millis();
  md5.begin();
  SerialPrintln("Rescue install started (md5 " + md5Param + ")");
  return true;
}

bool factoryWriteChunk(const uint8_t* data, size_t len, size_t streamOffset) {
  if (target == nullptr || lastError.length() > 0) return false;
  if (len == 0) return true;
  // The caller's stream offset must match our cursor: a chunk from any other
  // request (or delivered out of order) fails the install loudly instead of
  // scrambling bytes into the slot. erasedUpTo > 0 by then, so fail() leaves
  // the header sector erased — nothing bootable can result.
  if (streamOffset != writeOffset) {
    return fail("out-of-order upload chunk (concurrent install?)");
  }
  lastChunkMs = millis();
  if (writeOffset + len > target->size) {
    return fail("image larger than the factory slot (" +
                String((unsigned)target->size) + " bytes)");
  }
  // First bytes must look like an app image (magic 0xE9) — reject a wrong
  // file before erasing/writing 2 MB of flash. erasedUpTo is still 0 here,
  // so fail() leaves a previous image untouched.
  if (writeOffset == 0) {
    if (data[0] != 0xE9) {
      return fail("not an app image (bad magic byte)");
    }
    // The install now owns the slot: erase the header sector up front so the
    // previous image stops validating before any new byte lands. The new
    // header only appears in factoryWriteEnd(), after the MD5 verdict.
    esp_err_t err = esp_partition_erase_range(target, 0, SECTOR);
    if (err != ESP_OK) {
      // erasedUpTo is still 0 here, so fail() leaves flash alone.
      return fail(String("flash erase: ") + esp_err_to_name(err));
    }
    erasedUpTo = SECTOR;
  }
  FactoryChunkPlan plan = planFactoryChunk(writeOffset, len, SECTOR);
  if (plan.headerBytes > 0) {
    memcpy(headerSector + writeOffset, data, plan.headerBytes);
  }
  if (plan.flashBytes > 0) {
    // Lazy erase, sector granularity, ahead of the write window.
    if (plan.eraseTo > erasedUpTo) {
      esp_err_t err =
          esp_partition_erase_range(target, erasedUpTo, plan.eraseTo - erasedUpTo);
      if (err != ESP_OK) {
        return fail(String("flash erase: ") + esp_err_to_name(err));
      }
      erasedUpTo = plan.eraseTo;
    }
    esp_err_t err = esp_partition_write(target, plan.flashOffset,
                                        data + plan.headerBytes, plan.flashBytes);
    if (err != ESP_OK) {
      return fail(String("flash write: ") + esp_err_to_name(err));
    }
  }
  // MD5 covers the whole stream, held-back header included.
  // MD5Builder::add takes uint16_t — feed large chunks in bounded slices.
  size_t added = 0;
  while (added < len) {
    size_t n = len - added;
    if (n > 32768) n = 32768;
    md5.add(const_cast<uint8_t*>(data) + added, (uint16_t)n);
    added += n;
  }
  writeOffset += len;
  return true;
}

bool factoryWriteEnd() {
  if (target == nullptr || lastError.length() > 0) return false;
  if (writeOffset == 0) {
    return fail("empty upload");
  }
  md5.calculate();
  String actual = md5.toString();
  if (actual != expectedMd5) {
    return fail("md5 mismatch (upload corrupted): got " + actual);
  }
  // Commit: the digest checked out, so the held-back header sector may land.
  size_t headerLen = (writeOffset < SECTOR) ? writeOffset : SECTOR;
  esp_err_t err = esp_partition_write(target, 0, headerSector, headerLen);
  if (err != ESP_OK) {
    return fail(String("flash write (header commit): ") + esp_err_to_name(err));
  }
  esp_app_desc_t desc;
  if (esp_ota_get_partition_description(target, &desc) != ESP_OK) {
    return fail("written image has no valid app descriptor");
  }
  SerialPrintln(String("Rescue image installed into the factory slot (") +
                desc.version + ", " + String((unsigned)writeOffset) +
                " bytes)");
  target = nullptr;
  installInProgress = false;
  return true;
}

String factoryWriteError() { return lastError; }

bool rescueBootArm() {
  const esp_partition_t* otadata = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, nullptr);
  if (otadata == nullptr) return false;
  esp_err_t err = esp_partition_erase_range(otadata, 0, otadata->size);
  if (err != ESP_OK) {
    SerialPrintln(String("rescue-boot otadata erase failed: ") +
                  esp_err_to_name(err));
    return false;
  }
  SerialPrintln(F("otadata erased — next boot enters the rescue image"));
  return true;
}
