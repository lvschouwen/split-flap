// FollowerImageStore glue (#304 Part B) — LittleFS + MD5 for the one staged
// follower image. Header owns the rationale + writer discipline. Bench-tier
// (LittleFS + FreeRTOS; not native-buildable). Pure bits: FollowerImagePolicy.h.

#include "FollowerImageStore.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <MD5Builder.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "FlashLog.h"          // flashLogAvailable() — LittleFS mount owner
#include "FollowerImagePolicy.h"  // followerImageChunkOk
#include "HelpersSerialHandling.h"
#include "LargeAlloc.h"

// All fields guarded by imgMutex except the accumulator buffer, which only
// the single async upload handler touches between begin and end.
static SemaphoreHandle_t imgMutex = nullptr;

static bool storedPresent = false;
static String storedRev;
static bool relayActive = false;

// Accumulator (async handler context, one upload at a time).
static bool accumulating = false;
static uint8_t* accBuf = nullptr;
static size_t accLen = 0;
static String accMd5;   // expected, lower-hex 32
// #419: last begin/chunk progress. An upload whose client dies mid-body
// never reaches writeEnd, which used to leave `accumulating` (and the PSRAM
// buffer) wedged until reboot — and the store is the ONLY supported esp01
// update path. The flush tick reclaims after this stall window; 30 s
// mirrors #313's OTA_STALL_TIMEOUT_MS on /firmware/master.
static const uint32_t FOLLOWER_IMAGE_STALL_TIMEOUT_MS = 30000UL;
static uint32_t accLastProgressMs = 0;
static String accRev;
static String lastError;

// Flush handoff: the accumulator hands its buffer to netTask.
static bool flushPending = false;
static uint8_t* flushBuf = nullptr;
static size_t flushLen = 0;
static String flushRev;

struct ImgLock {
  ImgLock() { xSemaphoreTake(imgMutex, portMAX_DELAY); }
  ~ImgLock() { xSemaphoreGive(imgMutex); }
  ImgLock(const ImgLock&) = delete;
  ImgLock& operator=(const ImgLock&) = delete;
};

void followerImageStoreInit() {
  static bool attempted = false;
  if (attempted) return;
  attempted = true;
  imgMutex = xSemaphoreCreateMutex();
  if (imgMutex == nullptr) {
    SerialPrintln(F("FollowerImageStore: mutex alloc failed — relay disabled"));
    return;
  }
  if (!flashLogAvailable()) return;  // storage never mounted; queries stay false
  ImgLock lock;
  // Presence follows the image file alone; a missing/failed .rev just leaves
  // the rev label blank (the image is still usable — the push recomputes MD5).
  storedPresent = LittleFS.exists(FOLLOWER_IMAGE_PATH);
  if (storedPresent && LittleFS.exists(FOLLOWER_IMAGE_REV_PATH)) {
    File f = LittleFS.open(FOLLOWER_IMAGE_REV_PATH, FILE_READ);
    if (f) {
      storedRev = f.readString();
      storedRev.trim();
      f.close();
    }
  }
}

bool followerImageStored() {
  if (imgMutex == nullptr) return false;
  ImgLock lock;
  return storedPresent;
}

String followerImageStoredRev() {
  if (imgMutex == nullptr) return String();
  ImgLock lock;
  return storedRev;
}

bool followerImageTryClaimRelay() {
  if (imgMutex == nullptr) return false;
  ImgLock lock;
  // Busy = a stale/about-to-change file; claim atomically with the check so a
  // writeEnd can't slip flushPending true between check and set.
  if (accumulating || flushPending) return false;
  relayActive = true;
  return true;
}

void followerImageReleaseRelay() {
  if (imgMutex == nullptr) return;
  ImgLock lock;
  relayActive = false;
}

String followerImageWriteError() {
  if (imgMutex == nullptr) return String();
  ImgLock lock;
  return lastError;
}

// --- accumulator --------------------------------------------------------------------

static void accFree() {
  if (accBuf != nullptr) {
    free(accBuf);
    accBuf = nullptr;
  }
  accLen = 0;
  accumulating = false;
}

bool followerImageWriteBegin(const String& expectedMd5, const String& rev) {
  if (imgMutex == nullptr) return false;
  {
    ImgLock lock;
    lastError = "";
    if (accumulating || flushPending) {
      lastError = "another follower-image upload is in progress";
      return false;
    }
    if (!flashLogAvailable()) {
      lastError = "storage unavailable";
      return false;
    }
    if (expectedMd5.length() != 32) {
      lastError = "md5 query param must be a 32-char hex digest";
      return false;
    }
    accumulating = true;
    accMd5 = expectedMd5;
    accMd5.toLowerCase();
    accRev = rev;
    accLen = 0;
    accLastProgressMs = millis();  // #419: arms the stall reclaim
  }
  // Allocate outside the lock (largeAlloc may be slow); PSRAM-preferred.
  uint8_t* buf = (uint8_t*)largeAlloc(FOLLOWER_IMAGE_MAX_BYTES);
  ImgLock lock;
  if (buf == nullptr) {
    lastError = "out of memory for the follower image buffer";
    accumulating = false;
    return false;
  }
  if (!accumulating) {
    // #419: the stall reclaim fired during the unlocked alloc window (only
    // reachable if the allocator itself stalled >30 s) — committing accBuf
    // here would resurrect a dead session and leak this buffer forever.
    free(buf);
    lastError = "upload reclaimed during buffer allocation";
    return false;
  }
  accBuf = buf;
  return true;
}

bool followerImageWriteChunk(const uint8_t* data, size_t len, size_t streamOffset) {
  if (imgMutex == nullptr) return false;
  ImgLock lock;
  if (!accumulating || accBuf == nullptr) return false;
  if (!followerImageChunkOk(streamOffset, accLen, len, FOLLOWER_IMAGE_MAX_BYTES)) {
    lastError = "upload chunk out of order or too large";
    accFree();
    return false;
  }
  memcpy(accBuf + accLen, data, len);
  accLen += len;
  accLastProgressMs = millis();  // #419: progress resets the stall deadline
  return true;
}

bool followerImageWriteEnd() {
  if (imgMutex == nullptr) return false;
  ImgLock lock;
  if (!accumulating || accBuf == nullptr) {
    lastError = "no upload in progress";
    return false;
  }
  if (accLen == 0) {
    lastError = "empty upload";
    accFree();
    return false;
  }
  MD5Builder md5;
  md5.begin();
  md5.add(accBuf, accLen);
  md5.calculate();
  String got = md5.toString();
  got.toLowerCase();
  if (got != accMd5) {
    lastError = "md5 mismatch: got " + got + ", expected " + accMd5;
    accFree();
    return false;
  }
  // Hand the buffer to netTask; the accumulator relinquishes ownership.
  flushBuf = accBuf;
  flushLen = accLen;
  flushRev = accRev;
  flushPending = true;
  accBuf = nullptr;
  accLen = 0;
  accumulating = false;
  lastError = "";
  return true;
}

// --- netTask flush ------------------------------------------------------------------

void followerImageFlushTick() {
  if (imgMutex == nullptr) return;
  {
    // #419: reclaim a stalled upload — no chunk for the stall window means
    // the client is gone and writeEnd will never run. Freeing here (netTask)
    // is safe: the async handler only touches accBuf between begin and end,
    // and a late straggler chunk fails the `accumulating` check in
    // followerImageWriteChunk instead of writing through a dangling pointer.
    ImgLock lock;
    if (accumulating &&
        millis() - accLastProgressMs > FOLLOWER_IMAGE_STALL_TIMEOUT_MS) {
      accFree();
      lastError = "upload stalled — store reclaimed";
      SerialPrintln(F("FollowerImageStore: stalled upload reclaimed (#419)"));
    }
  }
  uint8_t* buf = nullptr;
  size_t len = 0;
  String rev;
  {
    ImgLock lock;
    if (!flushPending) return;
    if (relayActive) return;  // don't rewrite the file the relay is streaming
    buf = flushBuf;
    len = flushLen;
    rev = flushRev;
  }

  bool ok = false;
  bool revOk = false;
  File f = LittleFS.open(FOLLOWER_IMAGE_PATH, FILE_WRITE);
  if (f) {
    ok = (f.write(buf, len) == len);
    f.close();
  }
  if (ok) {
    File rf = LittleFS.open(FOLLOWER_IMAGE_REV_PATH, FILE_WRITE);
    if (rf) {
      revOk = (rf.print(rev) == rev.length());
      rf.close();
    }
    if (!revOk) LittleFS.remove(FOLLOWER_IMAGE_REV_PATH);  // no half-rev
    SerialPrintf("FollowerImageStore: stored %u bytes, rev %s\n",
                 (unsigned)len, revOk ? rev.c_str() : "(unknown — rev write failed)");
  } else {
    SerialPrintln(F("FollowerImageStore: flash write failed"));
    LittleFS.remove(FOLLOWER_IMAGE_PATH);  // no torn image left bootable
    LittleFS.remove(FOLLOWER_IMAGE_REV_PATH);
  }

  ImgLock lock;
  free(flushBuf);
  flushBuf = nullptr;
  flushLen = 0;
  flushPending = false;
  // Reflect the true on-flash state on BOTH outcomes — a failed write that was
  // overwriting a prior image must NOT keep claiming the old one is present,
  // or every eligibility check passes and every push then fails to read it.
  storedPresent = ok;
  storedRev = (ok && revOk) ? rev : String();
}
