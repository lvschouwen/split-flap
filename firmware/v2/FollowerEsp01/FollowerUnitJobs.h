#pragma once
// FollowerUnitJobs.h — the row's unit jobs: one staged job at a time, run by
// loop() (the only I2C context), its end read from the result slots. The
// wall link stages them (FollowerLink.cpp; which checks a job takes is
// FollowerLinkOps.h). loop() context only.

#include <stddef.h>
#include <stdint.h>

#include "BootInfo.h"
#include "FollowerOps.h"

enum class UnitOpStaged : uint8_t { Yes, Rescue, Busy, NoMemory };

// Stages one job for unitJobsLoopTick(); `seq` names it in the result slots.
UnitOpStaged unitOpStage(FollowerOpKind kind, uint8_t addr, long arg,
                         uint32_t& seq);
// A unit job is queued, running or still being waited on.
bool unitOpsBusy();
// A unit or bootloader update is queued or running: the board must not take
// a firmware upload, whose restart would strand the unit being flashed.
bool unitUpdateQueuedOrRunning();
const MaintResult& unitOpResult();
const SelfTestSlot& unitOpSelfTest();
const BootInfoSlot& unitOpBootInfo();
// The BOOT_SECTION_LEN bytes the dump job `seq` read; nullptr once they are
// no longer held.
const uint8_t* unitOpBootDumpBytes(uint32_t seq);

// The unit facts document into buf; returns its length. Size the buffer with
// followerHealthBufCap().
size_t unitsHealthJson(char* buf, size_t cap);

// loop(): runs the staged job, polls a running self-test, rescans the row
// once any bootloader window is over.
void unitJobsLoopTick();
