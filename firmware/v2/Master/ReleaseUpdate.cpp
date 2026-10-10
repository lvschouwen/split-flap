// ReleaseUpdate glue (#583). Header owns the rationale. Bench-tier: HTTPS,
// flash and FreeRTOS; the order of every step is ../release/ReleaseFetch.h,
// natively tested.

#include "ReleaseUpdate.h"

#include <Update.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <atomic>
#include <memory>
#include <new>

#include "BuildVersion.h"
#include "ClockPolicy.h"
#include "EventRecord.h"
#include "FactorySlot.h"
#include "FollowerImagePolicy.h"
#include "FollowerImageStore.h"
#include "HelpersSerialHandling.h"
#include "MqttService.h"
#include "ReflashPlan.h"
#include "ReleaseTarget.h"
#include "TaskWatchdog.h"
#include "Tasks.h"
#include "WallState.h"
#include "WebEndpoints.h"

namespace {

SemaphoreHandle_t mutex = nullptr;
struct Lock {
  Lock() { xSemaphoreTake(mutex, portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(mutex); }
  Lock(const Lock&) = delete;
  Lock& operator=(const Lock&) = delete;
};

// Under the mutex.
ReleaseStatus status;
String channel = RELEASE_CHANNEL_DEFAULT;
bool channelChanged = false;
uint32_t askedLookOp = 0;
uint32_t askedUpdateOp = 0;

std::atomic<bool> checkEnabled{true};
std::atomic<bool> busy{false};
std::atomic<bool> updating{false};

// Worker task only.
ReleaseSchedule schedule;
char lastFoundTag[RELEASE_TAG_MAX + 1] = {0};

// The download buffer is internal RAM: it is handed to the flash writers.
constexpr size_t DOWNLOAD_BUFFER = 4096;
// How long a stored row image may wait for netTask to write it (a row that
// is downloading the old one holds the file).
constexpr uint32_t ROW_IMAGE_WRITE_WAIT_MS = 240000UL;

void setStep(ReleaseStep step, uint32_t size = 0) {
  Lock lock;
  status.step = step;
  status.done = 0;
  status.size = size;
}

// The task watchdog is fed around every call that waits for the site: a name
// lookup, a connection and a read each wait up to RELEASE_HTTP_TIMEOUT_MS.
class Site : public ReleaseSite {
 public:
  int get(const char* url, uint8_t* out, size_t cap) override {
    wdtFeed();
    const int n = ReleaseSite::get(url, out, cap);
    wdtFeed();
    return n;
  }
  long open(const char* url) override {
    wdtFeed();
    const long length = ReleaseSite::open(url);
    wdtFeed();
    return length;
  }
  void progress(uint32_t done, uint32_t size) override {
    wdtFeed();
    Lock lock;
    status.done = done;
    status.size = size;
  }
};

// ---- the three writers -------------------------------------------------------------

struct RescueWriter : ReleaseWriter {
  ReleaseError begin(const ReleaseImage&) override {
    return factoryWriteBeginChecked() ? ReleaseError::Ok : ReleaseError::Busy;
  }
  bool write(const uint8_t* data, size_t len, uint32_t offset) override {
    return factoryWriteChunk(data, len, offset);
  }
  bool commit() override { return factoryWriteEnd(); }
  void abort() override { factoryWriteAbort(); }
};

String hexOf(const uint8_t* bytes, size_t n) {
  static const char digits[] = "0123456789abcdef";
  String out;
  out.reserve(2 * n);
  for (size_t i = 0; i < n; i++) {
    out += digits[bytes[i] >> 4];
    out += digits[bytes[i] & 0x0F];
  }
  return out;
}

struct RowImageWriter : ReleaseWriter {
  String md5;
  String heldFor;
  ReleaseError begin(const ReleaseImage& image) override {
    return followerImageWriteBegin(md5, image.rev, heldFor) ? ReleaseError::Ok
                                                            : ReleaseError::Busy;
  }
  bool write(const uint8_t* data, size_t len, uint32_t offset) override {
    return followerImageWriteChunk(data, len, offset);
  }
  bool commit() override { return followerImageWriteEnd(); }
  void abort() override { followerImageWriteAbort(); }
};

struct MasterWriter : ReleaseWriter {
  bool frozen = false;
  ReleaseError begin(const ReleaseImage& image) override {
    if (Update.isRunning()) Update.abort();  // what an upload that died left behind
    if (!Update.begin(image.size, U_FLASH)) return ReleaseError::Write;
    // As for an upload: no MQTT traffic while the slot is written.
    mqttStopForOta();
    frozen = true;
    return ReleaseError::Ok;
  }
  bool write(const uint8_t* data, size_t len, uint32_t) override {
    return Update.write(const_cast<uint8_t*>(data), len) == len;
  }
  // Makes the slot the next to start.
  bool commit() override { return Update.end(false); }
  void abort() override {
    if (Update.isRunning()) Update.abort();
    if (frozen) mqttResumeAfterOta();
    frozen = false;
  }
};

// ---- looking -----------------------------------------------------------------------

ReleaseError look(Site& site, ReleaseManifest& found) {
  String wanted;
  {
    Lock lock;
    wanted = channel;
    channelChanged = false;
  }
  const time_t now = time(nullptr);
  ReleaseError result = ReleaseError::NoClock;
  if (clockIsTimeSynced(now)) {
    std::unique_ptr<ReleaseLookBuffers> work(new (std::nothrow) ReleaseLookBuffers);
    result = work ? releaseLook(site, wanted.c_str(), *work, found) : ReleaseError::Memory;
  }
  schedule.done(millis(), result == ReleaseError::Ok);
  ReleaseLookState state;
  {
    Lock lock;
    // A channel chosen while this look ran: what it found is of the old one.
    if (channelChanged) return ReleaseError::Channel;
    releaseStatusFold(status, result, found, GIT_COMMIT_TIME, (uint32_t)now);
    state = status.look;
  }
  if (result != ReleaseError::Ok) {
    SerialPrintf("release: the look at the %s channel failed: %s\n", wanted.c_str(),
                 releaseErrorText(result));
    return result;
  }
  SerialPrintf("release: %s (master %s, row image %s, rescue %s) is %s\n", found.tag,
               found.master.rev, found.row.rev, found.rescue.rev,
               state == ReleaseLookState::Newer ? "newer than what runs" : "not newer");
  if (state == ReleaseLookState::Newer && strcmp(lastFoundTag, found.tag) != 0) {
    strlcpy(lastFoundTag, found.tag, sizeof(lastFoundTag));
    eventRecord(EventKind::ReleaseFound, 0, "", 0, eventRevNumber(found.master.rev),
                found.commitTime);
  }
  return result;
}

// ---- updating ----------------------------------------------------------------------

// Why not, nullptr when the update went through to the restart.
const char* failed(char* out, size_t cap, const char* what, ReleaseError why,
                   const String& detail = String()) {
  if (detail.length() > 0) {
    snprintf(out, cap, "%s: %s (%s)", what, releaseErrorText(why), detail.c_str());
  } else {
    snprintf(out, cap, "%s: %s", what, releaseErrorText(why));
  }
  return out;
}

const char* update(char* why, size_t cap, bool& nothingToDo) {
  nothingToDo = false;
  if (reflashInProgress(displaySnapshotGet().reflash) || wallUnitUpdateRunning()) {
    return "a unit update is running";
  }
  Site site;
  ReleaseManifest release;
  setStep(ReleaseStep::Looking);
  // Always a fresh look: never an install from what was found a day ago.
  const ReleaseError looked = look(site, release);
  if (looked != ReleaseError::Ok) return failed(why, cap, "the look", looked);
  if (!releaseNewer(release, GIT_COMMIT_TIME)) {
    nothingToDo = true;
    return nullptr;
  }
  uint8_t* buffer = (uint8_t*)heap_caps_malloc(DOWNLOAD_BUFFER, MALLOC_CAP_INTERNAL);
  if (buffer == nullptr) return failed(why, cap, "the download", ReleaseError::Memory);
  std::unique_ptr<uint8_t, void (*)(void*)> owned(buffer, heap_caps_free);

  const bool rescueDiffers = releaseImageDiffers(release.rescue, rescueSlotCurrent().rev);
  const bool rowDiffers = followerImageReleaseStores(
      followerImageStoredRev().c_str(), followerImageStoredHeldFor().c_str(), GIT_REV,
      release.row.rev, release.master.rev);
  // Every image first: the rescue slot is erased as its download begins, and
  // that is not worth it for a release whose master image is not there.
  const struct {
    bool wanted;
    const ReleaseImage& image;
    const char* what;
  } wanted[] = {{rescueDiffers, release.rescue, "the rescue image"},
                {rowDiffers, release.row, "the row image"},
                {true, release.master, "the master's firmware"}};
  for (const auto& w : wanted) {
    if (!w.wanted) continue;
    const ReleaseError e = releaseReachable(site, release.channel, w.image);
    if (e != ReleaseError::Ok) return failed(why, cap, w.what, e);
  }

  SerialPrintf("release: updating to %s\n", release.tag);
  eventRecord(EventKind::UpdateStarted, 0, "", 0, eventRevNumber(release.master.rev),
              release.commitTime);

  if (rescueDiffers) {
    setStep(ReleaseStep::Rescue, release.rescue.size);
    RescueWriter writer;
    const ReleaseError e =
        releaseInstall(site, release.channel, release.rescue, writer, buffer, DOWNLOAD_BUFFER);
    if (e != ReleaseError::Ok) {
      return failed(why, cap, "the rescue image", e, factoryWriteError());
    }
    webNoteRescueInstalled(release.rescue.rev);
  }

  if (rowDiffers) {
    setStep(ReleaseStep::RowImage, release.row.size);
    RowImageWriter writer;
    writer.md5 = hexOf(release.rowMd5, sizeof(release.rowMd5));
    writer.heldFor = release.master.rev;
    const ReleaseError e =
        releaseInstall(site, release.channel, release.row, writer, buffer, DOWNLOAD_BUFFER);
    if (e != ReleaseError::Ok) {
      return failed(why, cap, "the row image", e, followerImageWriteError());
    }
    // On flash before this board restarts.
    const uint32_t since = millis();
    while (followerImageWritePending()) {
      if (millis() - since > ROW_IMAGE_WRITE_WAIT_MS) {
        return "the row image: a row board kept the old one open, try again";
      }
      wdtFeed();
      vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (followerImageStoredRev() != release.row.rev) {
      return "the row image: it could not be stored";
    }
  }

  setStep(ReleaseStep::Master, release.master.size);
  MasterWriter writer;
  const ReleaseError e =
      releaseInstall(site, release.channel, release.master, writer, buffer, DOWNLOAD_BUFFER);
  if (e != ReleaseError::Ok) {
    return failed(why, cap, "the master's firmware", e,
                  Update.hasError() ? String(Update.errorString()) : String());
  }
  SerialPrintf("release: %s is written and checked; restarting into it\n", release.master.rev);
  webNoteIntendedVersion(release.master.rev);
  strlcpy(why, release.tag, cap);  // handed back as what was installed
  return nullptr;
}

void runUpdate(uint32_t op) {
  {
    Lock lock;
    status.op = op;
  }
  char why[WALL_OP_DETAIL_MAX] = {0};
  bool nothingToDo = false;
  const char* refusal = update(why, sizeof(why), nothingToDo);
  if (refusal != nullptr) {
    SerialPrintf("release: the update failed: %s\n", refusal);
    wallOpFinish(op, false, refusal);
  } else if (nothingToDo) {
    wallOpFinish(op, true, "nothing newer than what runs");
  } else {
    char detail[WALL_OP_DETAIL_MAX];
    snprintf(detail, sizeof(detail), "%s installed, restarting", why);
    setStep(ReleaseStep::Restarting);
    wallOpFinish(op, true, detail);
    webRequestReboot("update from a release");
    // Still running, for every gate, until the restart.
    return;
  }
  {
    Lock lock;
    status.op = 0;
    status.step = ReleaseStep::None;
    status.done = status.size = 0;
  }
  updating.store(false);
  busy.store(false);
}

void runLook(uint32_t op) {
  Site site;
  ReleaseManifest found;
  const ReleaseError result = look(site, found);
  // The deepest the worker goes (a TLS handshake): how much of its stack was left.
  SerialPrintf("release: the worker's stack kept %u bytes free\n",
               (unsigned)uxTaskGetStackHighWaterMark(nullptr));
  if (op != 0) {
    if (result != ReleaseError::Ok) {
      wallOpFinish(op, false, releaseErrorText(result));
    } else {
      char detail[WALL_OP_DETAIL_MAX];
      snprintf(detail, sizeof(detail), "%s is %s", found.tag,
               releaseNewer(found, GIT_COMMIT_TIME) ? "newer than what runs"
                                                    : "not newer than what runs");
      wallOpFinish(op, true, detail);
    }
  }
  busy.store(false);
}

}  // namespace

void releaseInit(bool check, const String& wanted) {
  mutex = xSemaphoreCreateMutex();
  if (mutex == nullptr) {
    Serial.println(F("FATAL: release mutex allocation failed"));
    abort();
  }
  checkEnabled.store(check);
  channel = wanted;
}

void releaseSetSettings(bool check, const String& wanted) {
  checkEnabled.store(check);
  Lock lock;
  if (channel == wanted) return;
  channel = wanted;
  channelChanged = true;
  const uint32_t op = status.op;
  const ReleaseStep step = status.step;
  status = ReleaseStatus();
  status.op = op;
  status.step = step;
}

ReleaseStatus releaseStatusGet() {
  Lock lock;
  return status;
}

bool releaseNewerTag(char* out, size_t cap) {
  Lock lock;
  if (status.look != ReleaseLookState::Newer) return false;
  strlcpy(out, status.release.tag, cap);
  return true;
}

bool releaseCheckEnabled() { return checkEnabled.load(); }
bool releaseBusy() { return busy.load(); }
bool releaseUpdateRunning() { return updating.load(); }

bool releaseAskLook(uint32_t op) {
  if (busy.exchange(true)) return false;
  Lock lock;
  askedLookOp = op;
  return true;
}

bool releaseAskUpdate(uint32_t op) {
  if (busy.exchange(true)) return false;
  updating.store(true);
  Lock lock;
  askedUpdateOp = op;
  return true;
}

void releaseTick() {
  uint32_t lookOp, updateOp;
  bool reset;
  {
    Lock lock;
    lookOp = askedLookOp;
    updateOp = askedUpdateOp;
    askedLookOp = askedUpdateOp = 0;
    reset = channelChanged;
  }
  if (reset) {
    schedule.reset();
    lastFoundTag[0] = 0;
  }
  if (updateOp != 0) return runUpdate(updateOp);
  if (lookOp != 0) return runLook(lookOp);
  if (!schedule.due(millis(), clockIsTimeSynced(time(nullptr)), checkEnabled.load())) return;
  // The look by itself: skipped, not queued, while one that was asked for runs.
  if (busy.exchange(true)) return;
  runLook(0);
}
