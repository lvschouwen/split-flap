// RescueReleaseJob glue (#583). Header owns the rationale. Bench-tier:
// HTTPS, flash and FreeRTOS; what it does in which order is RescueRelease.h
// and ../release/ReleaseFetch.h, natively tested.

#include "RescueReleaseJob.h"

#include <Update.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <atomic>

#include "ReleaseTarget.h"

namespace {

SemaphoreHandle_t mutex = nullptr;
struct Lock {
  Lock() { xSemaphoreTake(mutex, portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(mutex); }
  Lock(const Lock&) = delete;
  Lock& operator=(const Lock&) = delete;
};

// Under the mutex.
RescueReleaseStatus status;
char askedTag[RELEASE_TAG_MAX + 1] = {0};  // empty = a look

std::atomic<bool> running{false};
std::atomic<bool> installing{false};
std::atomic<bool> asked{false};

// The download buffer is internal RAM: it is handed to the flash writer.
constexpr size_t DOWNLOAD_BUFFER = 4096;
// A download that is alive but this slow is given up: it holds the slot,
// and with it every upload. A release's master image takes under a minute.
constexpr uint32_t DOWNLOAD_MAX_MS = 10UL * 60UL * 1000UL;

class Site : public ReleaseSite {
 public:
  long open(const char* url) override {
    openedMs_ = millis();
    return ReleaseSite::open(url);
  }
  int read(uint8_t* out, size_t cap) override {
    if (millis() - openedMs_ > DOWNLOAD_MAX_MS) return -1;
    return ReleaseSite::read(out, cap);
  }
  void progress(uint32_t done, uint32_t size) override {
    Lock lock;
    status.done = done;
    status.size = size;
  }

 private:
  uint32_t openedMs_ = 0;
};

// The next update partition: app0 when this image runs from the factory
// slot, the slot an upload writes.
struct SlotWriter : ReleaseWriter {
  ReleaseError begin(const ReleaseImage& image) override {
    if (Update.isRunning()) Update.abort();  // what an upload that died left behind
    return Update.begin(image.size, U_FLASH) ? ReleaseError::Ok : ReleaseError::Write;
  }
  bool write(const uint8_t* data, size_t len, uint32_t) override {
    return Update.write(const_cast<uint8_t*>(data), len) == len;
  }
  // Makes the slot the next to start. It starts unconfirmed, as after an
  // upload: an image that fails its health check is rolled back.
  bool commit() override { return Update.end(false); }
  void abort() override {
    if (Update.isRunning()) Update.abort();
  }
};

// The job works on its own copy and the shared one is replaced whole at the
// end: the page never reads a release half written.
void publish(const RescueReleaseStatus& mine) {
  Lock lock;
  status = mine;
}

}  // namespace

void rescueReleaseInit() {
  mutex = xSemaphoreCreateMutex();
  if (mutex == nullptr) {
    Serial.println(F("FATAL: release mutex allocation failed"));
    abort();
  }
}

bool rescueReleaseAsk(const char* channel, const char* tag) {
  if (running.exchange(true)) return false;
  const bool install = tag != nullptr;
  installing.store(install);
  Lock lock;
  strlcpy(askedTag, install ? tag : "", sizeof(askedTag));
  rescueReleaseStarted(status,
                       install ? RescueReleaseState::Installing : RescueReleaseState::Looking,
                       channel);
  asked.store(true);
  return true;
}

bool rescueReleaseRunning() { return running.load(); }
bool rescueReleaseInstalling() { return installing.load(); }

RescueReleaseStatus rescueReleaseStatusGet() {
  Lock lock;
  return status;
}

bool rescueReleaseTick() {
  if (!asked.exchange(false)) return false;
  char tag[sizeof(askedTag)];
  RescueReleaseStatus mine;
  {
    Lock lock;
    strlcpy(tag, askedTag, sizeof(tag));
    mine = status;
  }
  const bool install = tag[0] != 0;
  Site site;
  ReleaseError result;
  if (install) {
    uint8_t* buffer = (uint8_t*)heap_caps_malloc(DOWNLOAD_BUFFER, MALLOC_CAP_INTERNAL);
    if (buffer == nullptr) {
      result = ReleaseError::Memory;
      mine.state = RescueReleaseState::Failed;
      mine.error = result;
    } else {
      SlotWriter slot;
      result = rescueReleaseInstall(site, mine.channel, tag, slot, buffer, DOWNLOAD_BUFFER, mine);
      heap_caps_free(buffer);
    }
  } else {
    result = rescueReleaseLook(site, mine.channel, mine);
  }
  publish(mine);
  // The deepest loop() goes (a TLS handshake): how much of its stack was left.
  Serial.printf("release: %s on the %s channel: %s%s%s (stack kept %u bytes free)\n",
                install ? "install" : "look", mine.channel, releaseErrorText(result),
                mine.tag[0] ? ", " : "", mine.tag,
                (unsigned)uxTaskGetStackHighWaterMark(nullptr));
  if (mine.state == RescueReleaseState::Installed) {
    Serial.printf("release: master %s is written and checked; restarting into it\n",
                  mine.masterRev);
    // Still running, for every gate, until the restart.
    return true;
  }
  installing.store(false);
  running.store(false);
  return false;
}
