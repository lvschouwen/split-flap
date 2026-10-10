#pragma once
// ReleaseUpdate — the master's look for a release and its update from one
// (#583; docs/superpowers/specs/2026-10-10-release-update-design.md, section
// 6). Rules: ReleasePolicy.h (when to look, what is known) and
// ../release/ReleaseFetch.h (the order of verify, write, activate).
//
// Everything that talks to the release site runs on the worker task
// (releaseTick): its stack is internal RAM, which a task that writes flash
// needs, and a slow site stalls nothing but the worker.
//
// It adds no flash writer. An update feeds the three that the uploads use:
// the factory slot (FactorySlot), the row image store (FollowerImageStore,
// held until this master runs the release) and the other app slot (Update).
// Each is fed by one stream at a time: an update is started only while no
// upload runs, and while it runs every upload route answers 409
// (releaseUpdateRunning(), gated by tests/test_release_glue.py). Both sides
// decide on the web server's task, so neither can slip past the other.

#include <Arduino.h>

#include "ReleasePolicy.h"

// setup(), before tasksInit().
void releaseInit(bool check, const String& channel);
// netTask, when a setting changed. Another channel forgets what was found.
void releaseSetSettings(bool check, const String& channel);

// Worker task, every pass.
void releaseTick();

// Any task: a copy of what is known.
ReleaseStatus releaseStatusGet();
// The tag of a release newer than what runs; false when none is known.
bool releaseNewerTag(char* out, size_t cap);
// The settings as the worker last saw them, for the reads.
bool releaseCheckEnabled();

// Web server's task. A look or an update that was asked for and has not
// ended; one at a time.
bool releaseBusy();
// An update that was asked for and has not ended. Stays set through the
// restart it ends in.
bool releaseUpdateRunning();
// False when one is running already. `op` is the job to finish.
bool releaseAskLook(uint32_t op);
bool releaseAskUpdate(uint32_t op);
