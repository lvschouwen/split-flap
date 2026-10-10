#pragma once
// RescueReleaseJob — the rescue image's look for a release and its install
// of that release's master image (#583). Rules: RescueRelease.h; the site
// and the signature check: ../release/ReleaseTarget.h.
//
// A job is asked for on the web server's task and runs in loop(): a look
// and a download wait for the release site, and loop()'s stack is internal
// RAM, which writing flash needs. One job at a time, and an install never
// next to an upload: both write app0 through the one Update session, and
// both are admitted on the web server's task, so neither can slip past the
// other. A look writes nothing and holds nothing back.

#include <Arduino.h>

#include "RescueRelease.h"

// setup().
void rescueReleaseInit();

// Web server's task. False when a job is running already. `tag` is the
// release an install is for, nullptr for a look.
bool rescueReleaseAsk(const char* channel, const char* tag);
// A job that was asked for and has not ended.
bool rescueReleaseRunning();
// An install that was asked for and has not ended: the slot is its. One
// that went through stays so until the restart it ends in.
bool rescueReleaseInstalling();
// Any task: a copy of what is known.
RescueReleaseStatus rescueReleaseStatusGet();

// loop(). True once: an install went through and the board is to restart.
bool rescueReleaseTick();
