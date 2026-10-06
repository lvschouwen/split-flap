// FollowerImageStore glue (#304 Part B) — LittleFS + MD5 for the one staged
// follower image. Header owns the rationale + writer discipline. Bench-tier
// (LittleFS + FreeRTOS; not native-buildable). Pure bits: FollowerImagePolicy.h.

#include "FollowerImageStore.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <MD5Builder.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <atomic>

#include "FlashLog.h"          // flashLogAvailable() — LittleFS mount owner
#include "FollowerImagePolicy.h"  // followerImageChunkOk
#include "HelpersSerialHandling.h"
#include "LargeAlloc.h"

// All fields guarded by imgMutex except the accumulator buffer, which only
// the single async upload handler touches between begin and end.
static SemaphoreHandle_t imgMutex = nullptr;

static bool storedPresent = false;
static String storedRev;
static int relayClaims = 0;

// What a row is told about the stored image. Known at once for an image that
// was just uploaded; after a start it is read off flash by netTask.
static FollowerImageFacts facts;
static bool factsKnown = false;
static bool factsOwed = false;
// A read off flash that failed is tried again, this far apart.
static const uint32_t FACTS_RETRY_MS = 60000UL;
static uint32_t factsTriedAtMs = 0;
static bool factsTried = false;
static std::atomic<uint32_t> factsGeneration{0};

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
  factsOwed = storedPresent;
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
  relayClaims++;
  return true;
}

void followerImageReleaseRelay() {
  if (imgMutex == nullptr) return;
  ImgLock lock;
  if (relayClaims > 0) relayClaims--;
}

bool followerImageFacts(FollowerImageFacts& out) {
  if (imgMutex == nullptr) return false;
  ImgLock lock;
  if (!storedPresent || !factsKnown) return false;
  out = facts;
  return true;
}

uint32_t followerImageFactsGeneration() {
  return factsGeneration.load(std::memory_order_relaxed);
}

// gzip's two magic bytes: the packed image the ESP-01's boot copier unpacks.
static bool imageIsPacked(const uint8_t* head, size_t len) {
  return len >= 2 && head[0] == 0x1F && head[1] == 0x8B;
}

// netTask, after a start: size and checksum of the image as it is on flash.
static void readFactsOffFlash() {
  FollowerImageFacts read;
  bool ok = false;
  File f = LittleFS.open(FOLLOWER_IMAGE_PATH, FILE_READ);
  if (f) {
    static uint8_t chunk[2048];  // netTask only
    MD5Builder md5;
    md5.begin();
    int n;
    while ((n = f.read(chunk, sizeof(chunk))) > 0) {
      if (read.size == 0) read.packed = imageIsPacked(chunk, (size_t)n);
      md5.add(chunk, (size_t)n);
      read.size += (uint32_t)n;
    }
    ok = read.size > 0 && read.size == f.size();
    f.close();
    md5.calculate();
    md5.getBytes(read.md5);
  }
  ImgLock lock;
  factsTried = true;
  factsTriedAtMs = millis();
  if (!storedPresent) factsOwed = false;
  if (!ok || !storedPresent) return;
  factsOwed = false;
  strlcpy(read.rev, storedRev.c_str(), sizeof(read.rev));
  facts = read;
  factsKnown = true;
  factsGeneration.fetch_add(1, std::memory_order_relaxed);
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
    bool owed;
    {
      ImgLock lock;
      owed = factsOwed && !flushPending &&
             (!factsTried || millis() - factsTriedAtMs >= FACTS_RETRY_MS);
    }
    if (owed) readFactsOffFlash();
  }
  {
    ImgLock lock;
    if (!flushPending) return;
    if (relayClaims > 0) return;  // don't rewrite the file a row is reading
    buf = flushBuf;
    len = flushLen;
    rev = flushRev;
  }

  // The buffer is what was uploaded and checked: its facts need no read-back.
  FollowerImageFacts written;
  written.size = (uint32_t)len;
  written.packed = imageIsPacked(buf, len);
  {
    MD5Builder md5;
    md5.begin();
    md5.add(buf, len);
    md5.calculate();
    md5.getBytes(written.md5);
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
  strlcpy(written.rev, storedRev.c_str(), sizeof(written.rev));
  facts = written;
  factsKnown = ok;
  factsOwed = false;
  factsGeneration.fetch_add(1, std::memory_order_relaxed);
}
