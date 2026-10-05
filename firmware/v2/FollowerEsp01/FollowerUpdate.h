#pragma once
// FollowerUpdate.h — fetching the image the master offers over the wall link
// (wl.Update, #564) and storing it for the boot copier. Rules:
// FollowerUpdatePolicy.h. loop() context only.

#include <stdint.h>

#include "wall_link.pb.h"

struct FollowerUpdateResult {
  wl_UpdatePhase phase;  // UPDATE_INSTALLED or UPDATE_FAILED
  wl_UpdateReason reason;
  uint32_t detail;
};

// GET http://<host>/firmware/row, check it, store it. Blocks loop() for the
// whole download (the row does no unit work meanwhile, as during an upload);
// the web server keeps answering. After UPDATE_INSTALLED the caller restarts
// the board — nothing else may touch the updater until then.
FollowerUpdateResult updateDownloadAndInstall(const wl_Update& offer, const char* host);

// True from the start of a download until it failed; stays true once an
// image is stored. POST /firmware/master refuses meanwhile: both would write
// through the one updater.
bool updateDownloadActive();
