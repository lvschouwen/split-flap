#pragma once
// RescueRelease.h — what the rescue image does with a release (#583;
// docs/superpowers/specs/2026-10-10-release-update-design.md, section 8).
// Pure, natively tested by test_rescue_release.
//
// It takes the master image and nothing else: the rescue image cannot
// replace itself, and a row board gets its image from a master that runs.
// It has no clock and needs none: it says what the release is, the page
// shows what the slots hold, and the person decides.
//
// The order of verify, write and activate is ../release/ReleaseFetch.h. An
// install looks again by itself, so the signature is checked in the run
// that writes — and it writes only the release the person was shown: when
// the site has another one by then, it stops and shows that one.

#include <ArduinoJson.h>
#include <stdint.h>
#include <string.h>

#include <memory>
#include <new>

#include "ReleaseFetch.h"

enum class RescueReleaseState : uint8_t {
  Idle,
  Looking,
  Found,
  Installing,
  Installed,  // written and checked; the board restarts into it
  Failed,
};

inline const char* rescueReleaseStateName(RescueReleaseState s) {
  switch (s) {
    case RescueReleaseState::Idle: return "idle";
    case RescueReleaseState::Looking: return "looking";
    case RescueReleaseState::Found: return "found";
    case RescueReleaseState::Installing: return "installing";
    case RescueReleaseState::Installed: return "installed";
    case RescueReleaseState::Failed: return "failed";
  }
  return "?";
}

struct RescueReleaseStatus {
  RescueReleaseState state = RescueReleaseState::Idle;
  ReleaseError error = ReleaseError::Ok;
  char channel[8] = {0};
  char tag[RELEASE_TAG_MAX + 1] = {0};
  char notes[RELEASE_NOTES_MAX + 1] = {0};
  char masterRev[RELEASE_REV_MAX + 1] = {0};
  uint32_t done = 0;
  uint32_t size = 0;
  // Found, but not by a look: an install met another release than the one
  // it was asked for and wrote nothing.
  bool changed = false;
};

// A job begins: nothing of an earlier one is left to read.
inline void rescueReleaseStarted(RescueReleaseStatus& status, RescueReleaseState state,
                                 const char* channel) {
  status = RescueReleaseStatus();
  status.state = state;
  strncpy(status.channel, channel, sizeof(status.channel) - 1);
}

namespace rescuerelease {

inline void failed(RescueReleaseStatus& status, ReleaseError why) {
  status.state = RescueReleaseState::Failed;
  status.error = why;
}

inline void named(RescueReleaseStatus& status, const ReleaseManifest& release) {
  strcpy(status.tag, release.tag);
  strcpy(status.notes, release.notes);
  strcpy(status.masterRev, release.master.rev);
}

inline ReleaseError look(ReleaseHooks& site, const char* channel, ReleaseManifest& release) {
  std::unique_ptr<ReleaseLookBuffers> work(new (std::nothrow) ReleaseLookBuffers);
  if (!work) return ReleaseError::Memory;
  return releaseLook(site, channel, *work, release);
}

}  // namespace rescuerelease

// Looks for the release of `channel`. `status` ends as Found or Failed.
inline ReleaseError rescueReleaseLook(ReleaseHooks& site, const char* channel,
                                      RescueReleaseStatus& status) {
  std::unique_ptr<ReleaseManifest> release(new (std::nothrow) ReleaseManifest);
  const ReleaseError e =
      release ? rescuerelease::look(site, channel, *release) : ReleaseError::Memory;
  if (e != ReleaseError::Ok) {
    rescuerelease::failed(status, e);
    return e;
  }
  rescuerelease::named(status, *release);
  status.state = RescueReleaseState::Found;
  return e;
}

// Looks again, then downloads the master image of the release `tag` into
// `slot`. `status` ends as Installed (the caller restarts the board), as
// Found and changed when the site's release is another one (nothing is
// written), or as Failed.
inline ReleaseError rescueReleaseInstall(ReleaseHooks& site, const char* channel, const char* tag,
                                         ReleaseWriter& slot, uint8_t* buffer, size_t bufferLen,
                                         RescueReleaseStatus& status) {
  std::unique_ptr<ReleaseManifest> release(new (std::nothrow) ReleaseManifest);
  ReleaseError e = release ? rescuerelease::look(site, channel, *release) : ReleaseError::Memory;
  if (e == ReleaseError::Ok) {
    rescuerelease::named(status, *release);
    if (strcmp(release->tag, tag) != 0) {
      status.state = RescueReleaseState::Found;
      status.changed = true;
      return e;
    }
    status.size = release->master.size;
    e = releaseInstall(site, channel, release->master, slot, buffer, bufferLen);
  }
  if (e != ReleaseError::Ok) {
    rescuerelease::failed(status, e);
    return e;
  }
  status.state = RescueReleaseState::Installed;
  return e;
}

// May a look or an install start? status 0 = yes. `tag` is the release an
// install is for, nullptr for a look.
struct RescueReleaseRefusal {
  int status;
  const char* why;
};

inline RescueReleaseRefusal rescueReleaseRefusal(bool online, bool uploadRunning, bool jobRunning,
                                                 bool restartStaged, const char* channel,
                                                 const char* tag = nullptr) {
  if (channel == nullptr || releaseChannelDir(channel) == nullptr) {
    return {400, "No such channel (stable or test)"};
  }
  if (tag != nullptr && (tag[0] == 0 || strlen(tag) > RELEASE_TAG_MAX)) {
    return {400, "Which release? Look for the latest one first"};
  }
  if (!online) {
    return {409, "This board runs its own access point and has no internet — upload a file "
                 "instead"};
  }
  if (uploadRunning) return {409, "An upload is writing the slot — retry when it finishes"};
  if (jobRunning) return {409, "Already looking at or installing a release"};
  if (restartStaged) return {409, "The board is restarting"};
  return {0, ""};
}

// The longest answer: every text at its limit and every byte of it a
// control character, which JSON writes in six.
#define RESCUE_RELEASE_JSON_MAX 1536

// The `release` member of GET /rescue/status. False when it did not fit.
inline bool rescueReleaseJson(const RescueReleaseStatus& status, char* out, size_t cap) {
  JsonDocument doc;
  doc["state"] = rescueReleaseStateName(status.state);
  if (status.channel[0] != 0) doc["channel"] = (const char*)status.channel;
  if (status.tag[0] != 0) {
    doc["tag"] = (const char*)status.tag;
    doc["notes"] = (const char*)status.notes;
    doc["master"] = (const char*)status.masterRev;
  }
  if (status.changed) doc["changed"] = true;
  if (status.state == RescueReleaseState::Failed) doc["error"] = releaseErrorText(status.error);
  if (status.state == RescueReleaseState::Installing) {
    doc["done"] = status.done;
    doc["size"] = status.size;
  }
  if (doc.overflowed()) return false;
  const size_t n = serializeJson(doc, out, cap);
  return n > 0 && n < cap - 1;
}
